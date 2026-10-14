/*
 * sioplay - PS1 side. Pulls video frames from a Pico 2 on controller port 2
 * and plays them.
 *
 * Three frame formats (made by pc/make_video.py):
 *   - black & white, delta + run-length compressed (--mode bw, default)
 *   - 16 colours with a palette per keyframe, delta compressed (--mode color16)
 *   - raw 128x128 colour (--mode color): exactly 32768 bytes, no header
 * The compressed ones play full screen at the video's own frame rate.
 *
 * Toolchain: PSn00bSDK. Build: cmake --preset default && cmake --build build
 *
 * Wire protocol (PS1 is the master, Pico is the device on port 2):
 *
 *   PS1 sends     Pico replies        Pico ACKs?
 *   0x21          (anything)          yes   <- our made-up device address
 *   0x01          (anything)          yes   <- "give me a frame"
 *   0x00          0xA5 = ready        yes   <- anything else = not ready yet
 *   0x00          length, low byte    yes
 *   0x00          length, high byte   yes
 *   0x00 x length frame bytes         yes, except after the very last byte
 *
 * The Pico only moves on to the next frame once every byte has been read,
 * so this program always reads the whole frame, even one it can't show.
 *
 * How a frame gets on screen (kept deliberately simple - the same LoadImage
 * method as the first working version):
 *   1. bytes arrive over the controller port into rx[]
 *   2. decoded into pic[] (16-bit pixels, at the video's own size)
 *   3. enlarged 2x on the CPU into big[] if that still fits the screen
 *   4. LoadImage copies big[] into the hidden one of two screen buffers
 *   5. after the next vblank, and once it is time for this frame, the
 *      hidden buffer becomes visible
 *
 * A status line at the bottom shows what is going on:
 *   OK   frames shown          WAIT  Pico had no frame ready
 *   BUS  failed transfers      BAD   damaged / unknown frames
 * Set SHOW_STATUS to 0 to hide it.
 */

#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>
#include <string.h>
#include "bwvideo.h"
#include "palvideo.h"

/* ---- SIO0 (controller / memory card bus) hardware registers ---- */
#define JOY_DATA (*(volatile uint8_t  *)0x1f801040)
#define JOY_STAT (*(volatile uint32_t *)0x1f801044)
#define JOY_MODE (*(volatile uint16_t *)0x1f801048)
#define JOY_CTRL (*(volatile uint16_t *)0x1f80104a)
#define JOY_BAUD (*(volatile uint16_t *)0x1f80104e)
#define IRQ_MASK_REG (*(volatile uint32_t *)0x1f801074)

#define STAT_TX_READY  0x0001  /* TX FIFO has room              */
#define STAT_RX_READY  0x0002  /* a received byte is waiting    */
#define STAT_ACK_LOW   0x0080  /* /ACK line is low right now    */
#define STAT_ACK_IRQ   0x0200  /* device pulsed /ACK            */

#define CTRL_TX_EN     0x0001
#define CTRL_SELECT    0x0002  /* pull /ATT (select) low        */
#define CTRL_IRQ_ACK   0x0010  /* clear the ACK flag            */
#define CTRL_RESET     0x0040
#define CTRL_ACK_IRQEN 0x1000  /* latch /ACK pulses in JOY_STAT */
#define CTRL_PORT2     0x2000  /* use port 2 instead of 1       */

/* Bus clock = 33.8688 MHz / SIO_BAUD_RELOAD.
 *   0x88 (136) = ~250 kHz  stock speed, what Sony's controllers use
 *   0x44  (68) = ~500 kHz  2x
 *   0x22  (34) = ~1 MHz    4x  (the Pico's PIO code was simulated up to here)
 * Faster only helps if the Pico keeps up; try it once the basics work. */
#define SIO_BAUD_RELOAD 0x0088

/* ---- our protocol ---- */
#define DEV_ADDR       0x21
#define CMD_GET_FRAME  0x01
#define STATUS_READY   0xA5

#define SHOW_STATUS 1

/* ---- screen and video memory layout ----
 *   (0,0)   screen buffer 0, 320x240
 *   (0,240) screen buffer 1, 320x240
 *   (960,0) debug font */
#define SCREEN_W  320
#define SCREEN_H  240
#define MAX_PIC_W 256
#define MAX_PIC_H 240

#define RAW_W     128
#define RAW_H     128
#define RAW_BYTES (RAW_W * RAW_H * 2)
#define MAX_FRAME 65535

static uint8_t  rx[MAX_FRAME + 1];
static uint32_t pic32[MAX_PIC_W * MAX_PIC_H / 2];    /* decoded picture   */
static uint32_t big32[SCREEN_W * SCREEN_H / 2];      /* enlarged, 4-byte aligned for LoadImage */
#define pic ((uint16_t *)pic32)
#define big ((uint16_t *)big32)
static uint8_t  idx16[MAX_PIC_W * MAX_PIC_H];         /* 16-colour picture as colour numbers */
static uint16_t pal16[PAL_MAX_COLORS];

static DISPENV disp[2];
static DRAWENV draw[2];
static int     shown;               /* screen buffer currently visible */

/* status counters */
static int n_ok, n_wait, n_bus, n_bad;

/* ======================================================================
 * Controller-port bus
 * ====================================================================== */

static void delay(int n) {
	for (volatile int i = 0; i < n; i++);
}

static void sio_init(void) {
	IRQ_MASK_REG &= ~(1 << 7);  /* we poll the bus; no interrupt handler may
	                           steal our ACK flags */
	JOY_CTRL = CTRL_RESET;
	JOY_MODE = 0x000d;      /* 8-bit characters, clock multiplier x1 */
	JOY_BAUD = SIO_BAUD_RELOAD;
	JOY_CTRL = 0;
}

static void sio_select_port2(void) {
	JOY_CTRL = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQEN | CTRL_PORT2;
	JOY_CTRL |= CTRL_IRQ_ACK;  /* clear any stale ACK */
	delay(100);                /* give the Pico time to see /ATT go low */
}

static void sio_deselect(void) {
	JOY_CTRL = 0;
	delay(100);
}

/* Send one byte, return the byte received at the same time.
 * Returns -1 if the device didn't ACK (nothing connected / Pico stalled). */
static int sio_xfer(uint8_t tx, int wait_ack) {
	while (!(JOY_STAT & STAT_TX_READY));
	JOY_DATA = tx;
	while (!(JOY_STAT & STAT_RX_READY));
	int rxb = JOY_DATA;

	if (wait_ack) {
		int timeout = 3000;
		while (!(JOY_STAT & STAT_ACK_IRQ)) {
			if (--timeout == 0)
				return -1;
		}
		/* wait for the pulse to end, so it can't be counted twice */
		timeout = 3000;
		while (JOY_STAT & STAT_ACK_LOW) {
			if (--timeout == 0)
				return -1;
		}
		JOY_CTRL |= CTRL_IRQ_ACK;
	}
	return rxb;
}

/* Returns the frame length on success (bytes stored in dst, up to max),
 * 0 if no frame was ready, -1 on a bus error. */
static int receive_frame(uint8_t *dst, int max) {
	int b, len = -1;

	sio_select_port2();

	if (sio_xfer(DEV_ADDR, 1) < 0)         goto done;
	if (sio_xfer(CMD_GET_FRAME, 1) < 0)    goto done;
	if (sio_xfer(0x00, 1) != STATUS_READY) { len = 0; goto done; }

	if ((b = sio_xfer(0x00, 1)) < 0)       goto done;
	int n = b;
	if ((b = sio_xfer(0x00, 1)) < 0)       goto done;
	n |= b << 8;

	for (int i = 0; i < n; i++) {
		b = sio_xfer(0x00, i != n - 1);
		if (b < 0)
			goto done;
		if (i < max)
			dst[i] = (uint8_t)b;
	}
	len = n;

done:
	sio_deselect();
	return len;
}

/* ======================================================================
 * Display: two screen buffers, flipped right after a vblank
 * ====================================================================== */

static int font_id;

static void clear_buffers(void) {
	for (int i = 0; i < 2; i++) {
		draw[i].isbg = 1;               /* fill with background colour */
		PutDrawEnv(&draw[i]);
		DrawSync(0);
		draw[i].isbg = 0;               /* later PutDrawEnv: don't erase */
	}
}

static void draw_status(int buf) {
#if SHOW_STATUS
	PutDrawEnv(&draw[buf]);
	FntPrint(font_id, "OK %d  WAIT %d  BUS %d  BAD %d", n_ok, n_wait, n_bus, n_bad);
	FntFlush(font_id);
#else
	(void)buf;
#endif
}

static void display_init(void) {
	ResetGraph(0);

	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&disp[i], 0, i * SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i * SCREEN_H, SCREEN_W, SCREEN_H);
		setRGB0(&draw[i], 0, 0, 40);   /* dark blue background */
	}
	clear_buffers();
	PutDispEnv(&disp[0]);
	shown = 0;
	SetDispMask(1);

#if SHOW_STATUS
	FntLoad(960, 0);
	font_id = FntOpen(8, SCREEN_H - 14, SCREEN_W - 16, 10, 1, 64);
#endif
	draw_status(0);
}

/* Show pic[] (w x h pixels). fps = 0 means "as soon as possible". */
static void present(int w, int h, int fps) {
	static int last_W, last_H, last_fps;
	static uint32_t due;                     /* in units of vblank*fps */
	const int hz = (GetVideoMode() == MODE_PAL) ? 50 : 60;

	/* enlarge 2x if it fits the screen, otherwise show 1:1 */
	const int scale = (w * 2 <= SCREEN_W && h * 2 <= SCREEN_H) ? 2 : 1;
	const int W = w * scale, H = h * scale;
	const int x0 = (SCREEN_W - W) / 2, y0 = (SCREEN_H - H) / 2;

	if (W != last_W || H != last_H) {        /* new size: wipe old borders */
		clear_buffers();
		last_W = W;
		last_H = H;
	}

	if (scale == 2) {
		for (int y = 0; y < h; y++) {
			const uint16_t *s = pic + y * w;
			uint32_t *d = big32 + (y * 2) * (W / 2);
			for (int x = 0; x < w; x++)
				d[x] = s[x] | ((uint32_t)s[x] << 16);   /* pixel twice */
			memcpy(d + W / 2, d, W * 2);               /* row twice   */
		}
	} else {
		memcpy(big, pic, W * H * 2);
	}

	/* copy into the hidden buffer */
	const int back = shown ^ 1;
	RECT r = { x0, back * SCREEN_H + y0, W, H };
	LoadImage(&r, big32);
	DrawSync(0);
	draw_status(back);

	/* wait for this frame's moment, always at least until the next vblank
	 * so the switch never happens mid-picture. If we fell behind (a busy
	 * scene), carry on from now instead of rushing to catch up. */
	if (fps > 0) {
		const uint32_t now = (uint32_t)VSync(-1) * fps;
		if (fps != last_fps)
			due = now;
		due += hz;                           /* one frame = hz/fps vblanks */
		if (due < now)
			due = now;
		do {
			VSync(0);
		} while ((uint32_t)VSync(-1) * fps < due);
	} else {
		VSync(0);
	}
	last_fps = fps;

	PutDispEnv(&disp[back]);
	shown = back;
}

/* ======================================================================
 * main
 * ====================================================================== */

int main(int argc, const char **argv) {
	(void)argc; (void)argv;

	display_init();
	sio_init();
	printf("sioplay: waiting for frames on port 2\n");

	/* a delta frame only works on top of the right previous picture */
	int have_key = 0;              /* 0 = none, 'B' = bw picture, 'P' = 16-colour */
	int cur_w = 0, cur_h = 0;

	for (;;) {
		int len = receive_frame(rx, MAX_FRAME);
		if (len <= 0) {
			if (len < 0) n_bus++; else n_wait++;
			if (((n_bus + n_wait) % 20) == 0)
				draw_status(shown);         /* keep the counters fresh */
			VSync(0);                       /* try again shortly */
			continue;
		}

		bw_info_t  info;
		pal_info_t pi;
		uint16_t   newpal[PAL_MAX_COLORS];
		if (bw_parse_header(rx, len, &info) &&
		           !(info.width & 1) &&
		           info.width  <= MAX_PIC_W &&
		           info.height <= MAX_PIC_H) {
			/* black & white */
			if (!info.key &&
			    (have_key != 'B' || info.width != cur_w || info.height != cur_h)) {
				n_bad++;
				continue;           /* wait for the next keyframe */
			}
			if (bw_decode(pic, rx, len, &info) != 0) {
				have_key = 0;       /* damaged: hold until a keyframe */
				n_bad++;
				continue;
			}
			have_key = 'B';
			cur_w = info.width;
			cur_h = info.height;
			n_ok++;
			present(info.width, info.height, info.fps);

		} else if (pal_parse_header(rx, len, &pi, newpal) &&
		           !(pi.width & 1) &&
		           pi.width  <= MAX_PIC_W &&
		           pi.height <= MAX_PIC_H) {
			/* 16 colours */
			if (!pi.key &&
			    (have_key != 'P' || pi.width != cur_w || pi.height != cur_h)) {
				n_bad++;
				continue;
			}
			if (pal_decode(idx16, rx, len, &pi) != 0) {
				have_key = 0;
				n_bad++;
				continue;
			}
			if (pi.key)
				memcpy(pal16, newpal, sizeof pal16);
			pal_render(pic, idx16, pi.width * pi.height, pal16);
			have_key = 'P';
			cur_w = pi.width;
			cur_h = pi.height;
			n_ok++;
			present(pi.width, pi.height, pi.fps);

		} else if (len == RAW_BYTES) {
			/* raw 128x128 colour frame */
			memcpy(pic, rx, RAW_BYTES);
			have_key = 0;           /* a bw video must restart at a keyframe */
			n_ok++;
			present(RAW_W, RAW_H, 0);

		} else {
			n_bad++;                /* unknown data */
		}
	}
	return 0;
}
