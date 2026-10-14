/*
 * ps1-pico-video - Pico 2 firmware
 *
 * Plugs into PS1 controller port 2, reads video frames from an SD card and
 * hands them to the PS1 program (ps1/main.c) one frame at a time.
 *
 *   core 0: services the PS1 bus (must answer within microseconds, so it does
 *           nothing else)
 *   core 1: reads frames from video.bin on the SD card into two buffers
 *
 * Wire protocol (PS1 = master, every byte is exchanged in both directions):
 *
 *   exchange  PS1 sends   Pico replies        Pico ACKs?
 *   0         0x21        0xFF                yes, if address matches
 *   1         0x01        0xFF                yes, if command is GET_FRAME
 *   2         0x00        status (0xA5 ready) yes if ready
 *   3         0x00        length low byte     yes
 *   4         0x00        length high byte    yes
 *   5..       0x00        frame data          yes, except the last byte
 *
 * The Pico doesn't care what is inside a frame. Raw pixels, run-length
 * encoded Bad Apple, MDEC bitstream: that is between the PC converter and
 * the PS1 program. A frame is only marked as "used" after the PS1 has read
 * every byte of it; an aborted transfer just sends the same frame again.
 *
 * video.bin format (made by pc/make_video.py):
 *   repeated records of [u16 length, little endian][length bytes]
 *   The file loops when it reaches the end.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include "ff.h"
#include "sio0_dev.pio.h"

/* ---- PS1 port wiring (GPIO numbers, not Pico board pin numbers) --------
 *   PS1 port pin 1  DATA  -> GP5
 *   PS1 port pin 2  CMD   -> GP2
 *   PS1 port pin 4  GND   -> any Pico GND
 *   PS1 port pin 6  ATT   -> GP4
 *   PS1 port pin 7  CLK   -> GP3
 *   PS1 port pin 9  ACK   -> GP6
 *   Leave pin 3 (7.6V) and pin 5 (3.5V) unconnected; power the Pico by USB.
 */
#define PIN_CMD   2   /* PIO "in" base                     */
#define PIN_CLK   3   /* must be PIN_CMD + 1 (PIO in pin 1) */
#define PIN_ATT   4   /* select, active low, polled by CPU  */
#define PIN_DATA  5   /* open drain                         */
#define PIN_ACK   6   /* open drain                         */

#define SM_CLOCK_HZ 50000000.0f   /* PIO timing in sio0_dev.pio assumes this */

/* ---- protocol ---- */
#define DEV_ADDR       0x21
#define CMD_GET_FRAME  0x01
#define STATUS_READY   0xA5
#define STATUS_BUSY    0x00

#define SD_POWERUP_DELAY_MS 1000  /* wait after power-on before touching the card */
#define SD_RETRY_DELAY_MS    500  /* wait between mount attempts */

#define VIDEO_FILE     "video.bin"
#define MAX_FRAME      65535

typedef struct {
	uint32_t len;
	uint8_t  data[MAX_FRAME];
} frame_buf_t;

/* Double buffer shared between the cores. buf_full[i] is owned by core 1
 * while false and by core 0 while true. */
static frame_buf_t     bufs[2];
static volatile bool   buf_full[2];
static volatile uint32_t frames_sent;

static PIO  pio = pio0;
static uint sm;
static uint prog_offset;

/* ======================================================================
 * core 1: SD card -> frame buffers
 * ====================================================================== */

static void core1_halt(const char *msg, int err) {
	for (;;) {
		printf("ERROR: %s (FatFs code %d)\n", msg, err);
		sleep_ms(2000);
	}
}

static void core1_main(void) {
	static FATFS fs;
	static FIL   fil;

	/* Some cards need time after power-up before they answer, and the first
	 * attempt often fails. Wait, then keep retrying instead of giving up. */
	sleep_ms(SD_POWERUP_DELAY_MS);

	FRESULT fr;
	for (int attempt = 1;; attempt++) {
		fr = f_mount(&fs, "", 1);
		if (fr == FR_OK)
			break;
		printf("SD mount attempt %d failed (FatFs code %d), retrying...\n",
		       attempt, fr);
		if (fr == FR_NO_FILESYSTEM)
			core1_halt("SD card has no FAT32/exFAT file system", fr);
		f_unmount("");
		sleep_ms(SD_RETRY_DELAY_MS);
	}

	fr = f_open(&fil, VIDEO_FILE, FA_READ);
	if (fr != FR_OK)
		core1_halt("could not open " VIDEO_FILE, fr);
	if (f_size(&fil) < 3)
		core1_halt(VIDEO_FILE " is empty", 0);

	printf("SD ok, %s is %lu bytes\n", VIDEO_FILE, (unsigned long)f_size(&fil));

	int w = 0;
	uint32_t frames_read = 0;
	absolute_time_t next_status = get_absolute_time();

	for (;;) {
		while (buf_full[w]) {
			/* Status line every 3 s, so a terminal opened late still
			 * shows what is going on (USB drops output sent before the
			 * terminal is opened). */
			if (time_reached(next_status)) {
				next_status = make_timeout_time_ms(3000);
				if (frames_sent == 0)
					printf("SD ok (%s, %lu bytes). Waiting for the PS1...\n",
					       VIDEO_FILE, (unsigned long)f_size(&fil));
				else
					printf("Playing: %lu frames sent to PS1\n",
					       (unsigned long)frames_sent);
			}
		}

		uint8_t hdr[2];
		UINT got;
		fr = f_read(&fil, hdr, 2, &got);
		if (fr != FR_OK || got < 2) {          /* end of file: loop the video */
			f_lseek(&fil, 0);
			continue;
		}

		uint32_t len = hdr[0] | ((uint32_t)hdr[1] << 8);
		if (len == 0)
			continue;

		fr = f_read(&fil, bufs[w].data, len, &got);
		if (fr != FR_OK || got != len) {       /* truncated record: restart */
			f_lseek(&fil, 0);
			continue;
		}

		bufs[w].len = len;
		__dmb();                               /* data visible before flag */
		buf_full[w] = true;
		w ^= 1;
		frames_read++;
	}
}

/* ======================================================================
 * core 0: PS1 bus
 * ====================================================================== */

static void bus_init(void) {
	/* Inputs from the PS1. Pulls off: the PS1 drives these, and the RP2350
	 * input pad erratum (E9) makes the default pull-down a bad idea. */
	const uint inputs[] = { PIN_CMD, PIN_CLK, PIN_ATT };
	for (unsigned i = 0; i < 3; i++) {
		gpio_init(inputs[i]);
		gpio_set_dir(inputs[i], GPIO_IN);
		gpio_disable_pulls(inputs[i]);
	}

	/* Open-drain outputs, controlled by PIO. The PS1 has the pull-ups. */
	pio_gpio_init(pio, PIN_DATA);
	pio_gpio_init(pio, PIN_ACK);
	gpio_disable_pulls(PIN_DATA);
	gpio_disable_pulls(PIN_ACK);

	prog_offset = pio_add_program(pio, &sio0_dev_program);
	sm = pio_claim_unused_sm(pio, true);

	pio_sm_config c = sio0_dev_program_get_default_config(prog_offset);
	sm_config_set_in_pins(&c, PIN_CMD);
	sm_config_set_out_pins(&c, PIN_DATA, 1);
	sm_config_set_set_pins(&c, PIN_DATA, 1);
	sm_config_set_sideset_pins(&c, PIN_ACK);
	sm_config_set_in_shift(&c, true, false, 32);   /* LSB first, manual push */
	sm_config_set_out_shift(&c, true, false, 32);  /* LSB first, manual pull */
	sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / SM_CLOCK_HZ);

	const uint32_t od_mask = (1u << PIN_DATA) | (1u << PIN_ACK);
	pio_sm_set_pins_with_mask(pio, sm, 0, od_mask);     /* level = low  */
	pio_sm_set_pindirs_with_mask(pio, sm, 0, od_mask);  /* but released */

	pio_sm_init(pio, sm, prog_offset, &c);              /* stays disabled */
}

/* Stop the state machine and let go of both lines. Called when the PS1
 * deselects us, so a half-finished byte can't confuse the next transaction. */
static void bus_reset(void) {
	pio_sm_set_enabled(pio, sm, false);
	pio_sm_clear_fifos(pio, sm);
	pio_sm_restart(pio, sm);
	pio_sm_set_pindirs_with_mask(pio, sm, 0, (1u << PIN_DATA) | (1u << PIN_ACK));
	pio_sm_exec(pio, sm, pio_encode_jmp(prog_offset));
}

static inline void bus_queue_reply(uint8_t b) {
	pio_sm_put_blocking(pio, sm, (uint8_t)~b);   /* inverted, see .pio file */
}

static inline void bus_queue_ack(bool ack) {
	pio_sm_put_blocking(pio, sm, ack ? 1u : 0u);
}

/* What we send during exchange number `ex` (ex >= 2) of a GET_FRAME. */
static inline uint8_t reply_for(int ex, bool ready, const frame_buf_t *f) {
	if (ex == 2) return ready ? STATUS_READY : STATUS_BUSY;
	if (ex == 3) return (uint8_t)(f->len & 0xff);
	if (ex == 4) return (uint8_t)(f->len >> 8);
	return f->data[ex - 5];
}

static void __not_in_flash_func(bus_loop)(void) {
	int rd = 0;

	for (;;) {
		/* idle: wait for the PS1 to select port 2 */
		while (gpio_get(PIN_ATT))
			tight_loop_contents();

		bus_queue_reply(0xFF);              /* reply for exchange 0: nothing */
		pio_sm_set_enabled(pio, sm, true);

		int  ex = 0;
		bool ours = true, ready = false;
		const frame_buf_t *f = &bufs[rd];
		int  last = 0;

		for (;;) {
			/* Read select BEFORE checking the FIFO: the last byte can land
			 * just as the PS1 deselects, and it still has to be counted. */
			bool selected = !gpio_get(PIN_ATT);
			if (pio_sm_is_rx_fifo_empty(pio, sm)) {
				if (!selected)
					break;
				continue;
			}

			uint8_t rx   = (uint8_t)(pio_sm_get(pio, sm) >> 24);
			bool    ack  = false;
			uint8_t next = 0xFF;

			if (!ours) {
				/* someone else's conversation (e.g. a memory card in
				 * slot 2 shares this select line): stay silent */
			} else if (ex == 0) {
				ours = (rx == DEV_ADDR);
				ack  = ours;
			} else if (ex == 1) {
				ours = (rx == CMD_GET_FRAME);
				if (ours) {
					ready = buf_full[rd];
					__dmb();
					f    = &bufs[rd];
					last = 4 + (int)f->len;
					ack  = true;
					next = reply_for(2, ready, f);
				}
			} else if (!ready) {
				ours = false;               /* PS1 will give up and retry */
			} else if (ex == last) {
				/* PS1 has the whole frame: hand the buffer back */
				buf_full[rd] = false;
				rd ^= 1;
				frames_sent++;
#ifdef PICO_DEFAULT_LED_PIN
				gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
#endif
				ours = false;               /* no ACK after the last byte */
			} else {
				ack  = true;
				next = reply_for(ex + 1, ready, f);
			}

			bus_queue_ack(ack);
			bus_queue_reply(next);
			ex++;
		}

		bus_reset();                         /* PS1 deselected us */
	}
}

int main(void) {
	stdio_init_all();

#ifdef PICO_DEFAULT_LED_PIN
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

	bus_init();
	multicore_launch_core1(core1_main);

	printf("ps1-pico-video ready on controller port\n");
	bus_loop();
}
