/*
 * sdtest - SD card diagnostic for the ps1-pico-video wiring.
 *
 * Flash sdtest.uf2, open the Pico's USB serial port (any baud rate) and read
 * the report. It repeats every 5 seconds, so you can change a wire (with USB
 * unplugged!), plug back in and look again.
 *
 * Steps:
 *   1. Wire check  - every SD line must read high with a pull-up (a line
 *                    reading low is touching GND) and must not be shorted to
 *                    a neighbour.
 *   2. Card wakeup - raw SPI commands CMD0, CMD8, ACMD41, CMD58, each one
 *                    reported, so you see exactly where the card stops answering.
 *   3. Sector 0    - reads the first sector and says how the card is formatted.
 *   4. FatFs       - mounts the card the same way the real firmware does,
 *                    lists the files and checks video.bin.
 *
 * Wiring (same as the main firmware):
 *   MISO GP16 (pin 21)   CS GP17 (pin 22)   SCK GP18 (pin 24)   MOSI GP19 (pin 25)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "ff.h"
#include "f_util.h"

#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#define SD_SPI   spi0

#define SD_POWERUP_DELAY_MS 1000
#define SD_FIRMWARE_KHZ     1000   /* must match baud_rate in pico/hw_config.c */

static const struct { uint gpio; uint phys; const char *name; } lines[] = {
	{ PIN_MISO, 21, "MISO" },
	{ PIN_CS,   22, "CS"   },
	{ PIN_SCK,  24, "SCK"  },
	{ PIN_MOSI, 25, "MOSI" },
};
#define NLINES 4

static int problems;

static void fail(const char *what, const char *hint) {
	problems++;
	printf("   FAIL: %s\n", what);
	if (hint)
		printf("   -> %s\n", hint);
}

/* ======================================================================
 * 1. wire check
 * ====================================================================== */

static void wire_check(void) {
	printf("\n[1] Wire check\n");

	for (int i = 0; i < NLINES; i++) {
		gpio_init(lines[i].gpio);
		gpio_set_dir(lines[i].gpio, GPIO_IN);
		gpio_pull_up(lines[i].gpio);
	}
	sleep_ms(5);

	/* each line alone must read high with a pull-up */
	for (int i = 0; i < NLINES; i++) {
		bool high = gpio_get(lines[i].gpio);
		printf("   %-4s GP%u (pin %u): %s\n", lines[i].name, lines[i].gpio,
		       lines[i].phys, high ? "high, ok" : "LOW");
		if (!high)
			fail("this line is pulled to GND",
			     "it is touching GND or a GND pin: check for a wire on the "
			     "wrong pin (pin 23 is GND!) or on SD pin 3/6");
	}

	/* drive one line low at a time, the others must stay high */
	for (int i = 0; i < NLINES; i++) {
		gpio_put(lines[i].gpio, 0);
		gpio_set_dir(lines[i].gpio, GPIO_OUT);
		sleep_us(200);
		for (int j = 0; j < NLINES; j++) {
			if (j == i || gpio_get(lines[j].gpio))
				continue;                /* still high: not shorted */
			printf("   %s and %s are connected together\n",
			       lines[i].name, lines[j].name);
			fail("short between two lines",
			     "look for a solder bridge or two wires in the same "
			     "breadboard row");
		}
		gpio_set_dir(lines[i].gpio, GPIO_IN);
		sleep_us(200);
	}
}

/* ======================================================================
 * 2. raw SD commands
 * ====================================================================== */

static uint8_t xfer(uint8_t b) {
	uint8_t r;
	spi_write_read_blocking(SD_SPI, &b, &r, 1);
	return r;
}

static void cs(bool low) {
	gpio_put(PIN_CS, low ? 0 : 1);
	xfer(0xFF);
}

/* send a command, return the R1 response (0xFF = no answer) */
static uint8_t cmd(uint8_t n, uint32_t arg, uint8_t crc) {
	cs(false);
	cs(true);
	for (int i = 0; i < 1000 && xfer(0xFF) != 0xFF; i++);  /* wait not-busy */

	xfer(0x40 | n);
	xfer(arg >> 24); xfer(arg >> 16); xfer(arg >> 8); xfer(arg);
	xfer(crc);

	uint8_t r = 0xFF;
	for (int i = 0; i < 10 && ((r = xfer(0xFF)) & 0x80); i++);
	return r;
}

static bool card_init(bool *sdhc) {
	printf("\n[2] Card wakeup (raw SPI at 400 kHz)\n");

	spi_init(SD_SPI, 400 * 1000);
	spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
	gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
	gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
	gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
	gpio_pull_up(PIN_MISO);
	gpio_init(PIN_CS);
	gpio_set_dir(PIN_CS, GPIO_OUT);
	gpio_put(PIN_CS, 1);

	/* at least 74 clocks with CS high */
	for (int i = 0; i < 10; i++)
		xfer(0xFF);

	/* CMD0: go to idle / SPI mode, should answer 0x01 */
	uint8_t r = 0xFF;
	for (int tries = 0; tries < 10 && r != 0x01; tries++)
		r = cmd(0, 0, 0x95);
	printf("   CMD0  (reset)        -> 0x%02X %s\n", r,
	       r == 0x01 ? "ok" : "");
	if (r == 0xFF) {
		fail("the card does not answer at all",
		     "check: 3.3V on SD pin 4, GND on pin 3 (and 6), MISO on SD pin 7, "
		     "card orientation, card pushed fully in. Try swapping MISO/MOSI.");
		return false;
	}
	if (r == 0x00) {
		fail("card answers 0x00 (MISO stuck low)",
		     "MISO may be wired to GND or the card is damaged");
		return false;
	}
	if (r != 0x01) {
		fail("strange answer to CMD0",
		     "unstable wiring: shorten the wires, add a 100nF capacitor "
		     "between SD 3.3V and GND, add the 10k resistor CS->3.3V");
		return false;
	}

	/* CMD8: check voltage, tells SD v2 (SDHC) from v1 */
	r = cmd(8, 0x1AA, 0x87);
	bool v2 = false;
	if (r == 0x01) {
		uint8_t b[4];
		for (int i = 0; i < 4; i++) b[i] = xfer(0xFF);
		v2 = (b[2] == 0x01 && b[3] == 0xAA);
		printf("   CMD8  (voltage)      -> 0x01, echo %02X%02X %s\n", b[2], b[3],
		       v2 ? "ok (SD v2 card)" : "");
		if (!v2) {
			fail("bad CMD8 echo", "unstable wiring, see above");
			return false;
		}
	} else {
		printf("   CMD8  (voltage)      -> 0x%02X (old SD v1 card)\n", r);
	}

	/* ACMD41: start the card. The spec says up to 1 s, but some cards take
	 * longer the first time after power-up, so allow 3 s. */
	absolute_time_t until = make_timeout_time_ms(3000);
	do {
		cmd(55, 0, 0x65);
		r = cmd(41, v2 ? 0x40000000 : 0, 0x77);
	} while (r == 0x01 && !time_reached(until));
	printf("   ACMD41 (start)       -> 0x%02X %s\n", r, r == 0 ? "ok" : "");
	if (r != 0x00) {
		fail("the card never finished starting",
		     "usually not enough power: add a 10uF or 100nF capacitor at "
		     "the card, use pin 36 (3V3 OUT) for power, shorten wires. "
		     "Or the card doesn't support SPI mode: try another card.");
		return false;
	}

	/* CMD58: read OCR, find out if it's SDHC/SDXC (block addressed) */
	*sdhc = false;
	if (v2) {
		r = cmd(58, 0, 0xFD);
		uint8_t o[4];
		for (int i = 0; i < 4; i++) o[i] = xfer(0xFF);
		*sdhc = o[0] & 0x40;
		printf("   CMD58 (card type)    -> %s\n", *sdhc ? "SDHC/SDXC" : "SDSC");
	}
	cs(false);
	return true;
}

/* ======================================================================
 * 3. sector 0
 * ====================================================================== */

static const char *part_name(uint8_t t) {
	switch (t) {
	case 0x0B: case 0x0C: return "FAT32 (good)";
	case 0x07:            return "exFAT or NTFS";
	case 0x04: case 0x06: case 0x0E: return "FAT16 (good)";
	case 0x01:            return "FAT12 (good)";
	case 0xEE:            return "GPT disk";
	case 0x83:            return "Linux ext (NOT supported)";
	default:              return "unknown";
	}
}

/* Read one 512-byte block. Returns NULL on success, or what went wrong. */
static const char *read_block0(uint8_t *s) {
	uint8_t r = cmd(17, 0, 0xFF);
	if (r != 0x00) {
		cs(false);
		return "card refused the read command";
	}
	uint8_t tok = 0xFF;
	absolute_time_t until = make_timeout_time_ms(300);
	while ((tok = xfer(0xFF)) == 0xFF && !time_reached(until));
	if (tok != 0xFE) {
		cs(false);
		return "no data came back";
	}
	for (int i = 0; i < 512; i++) s[i] = xfer(0xFF);
	xfer(0xFF); xfer(0xFF);   /* CRC */
	cs(false);
	return NULL;
}

static bool read_sector0(void) {
	printf("\n[3] Reading sector 0 at different speeds\n");
	static uint8_t s[512], ref[512];
	static const uint32_t speeds_khz[] = { 400, 1000, 2000, 4000, 8000, 12000 };
	uint32_t best = 0;
	bool have_ref = false;

	for (unsigned k = 0; k < sizeof speeds_khz / sizeof speeds_khz[0]; k++) {
		spi_set_baudrate(SD_SPI, speeds_khz[k] * 1000);

		/* read 5 times; every read must succeed and give the same data */
		const char *err = NULL;
		for (int t = 0; t < 5 && !err; t++) {
			err = read_block0(s);
			if (!err && !have_ref) {
				memcpy(ref, s, 512);
				have_ref = true;
			} else if (!err && memcmp(s, ref, 512)) {
				err = "data different on each read";
			}
		}
		printf("   %5lu kHz: %s\n", (unsigned long)speeds_khz[k], err ? err : "ok");
		if (err)
			break;
		best = speeds_khz[k];
	}

	if (best == 0) {
		fail("cannot read data even at 400 kHz",
		     "check MISO (SD pin 7), add the 2nd GND (SD pin 6) and a 100nF "
		     "capacitor at the card, shorten the wires");
		return false;
	}
	printf("   fastest reliable speed: %lu kHz\n", (unsigned long)best);
	if (best < SD_FIRMWARE_KHZ) {
		printf("   the firmware uses %d kHz, faster than your wiring allows\n",
		       SD_FIRMWARE_KHZ);
		fail("wiring too slow for the firmware's SD speed",
		     "shorten the SD wires / add the capacitor, or tell me the "
		     "speed above and I'll lower it in the firmware");
	}

	spi_set_baudrate(SD_SPI, 400 * 1000);
	memcpy(s, ref, 512);

	if (s[510] != 0x55 || s[511] != 0xAA) {
		fail("sector 0 has no valid signature",
		     "the card is not formatted (or data is corrupted by bad wiring): "
		     "format it as FAT32 on your PC");
		return false;
	}

	if (!memcmp(&s[82], "FAT32", 5))       printf("   no partition table, FAT32 volume (good)\n");
	else if (!memcmp(&s[3], "EXFAT", 5))   printf("   no partition table, exFAT volume (good)\n");
	else if (!memcmp(&s[3], "NTFS", 4)) {  printf("   NTFS volume\n");
		fail("NTFS is not supported", "format the card as FAT32 or exFAT");
		return false;
	} else {
		uint8_t t = s[0x1C2];
		printf("   partition 1 type 0x%02X: %s\n", t, part_name(t));
		if (t == 0x00)
			fail("no partition found", "format the card as FAT32 on your PC");
	}
	printf("   ok\n");
	return true;
}

/* ======================================================================
 * 4. FatFs (same library as the main firmware)
 * ====================================================================== */

static void fatfs_check(void) {
	printf("\n[4] Mounting with FatFs (like the real firmware)\n");

	/* give the pins back so the library can set them up itself */
	spi_deinit(SD_SPI);
	for (int i = 0; i < NLINES; i++)
		gpio_set_function(lines[i].gpio, GPIO_FUNC_NULL);

	static FATFS fs;
	FRESULT fr = f_mount(&fs, "", 1);
	printf("   f_mount -> %s (%d)\n", FRESULT_str(fr), fr);
	if (fr != FR_OK) {
		if (fr == FR_NO_FILESYSTEM)
			fail("card works but has no FAT file system", "format it as FAT32");
		else
			fail("mount failed although raw test passed",
			     "the library runs the SPI faster: shorten the wires");
		return;
	}

	DIR dir;
	FILINFO fi;
	printf("   files in root:\n");
	if (f_opendir(&dir, "/") == FR_OK) {
		int n = 0;
		while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
			printf("     %s%s  %lu bytes\n", fi.fname,
			       (fi.fattrib & AM_DIR) ? "/" : "", (unsigned long)fi.fsize);
			n++;
		}
		if (!n)
			printf("     (empty)\n");
		f_closedir(&dir);
	}

	fr = f_stat("video.bin", &fi);
	if (fr != FR_OK) {
		fail("video.bin not found in the root",
		     "copy it to the top level of the card; check Windows didn't "
		     "name it video.bin.bin");
	} else if (fi.fsize % 32770 != 0) {
		printf("   video.bin: %lu bytes\n", (unsigned long)fi.fsize);
		fail("video.bin size is odd", "copy it again, the copy may be incomplete");
	} else {
		printf("   video.bin: %lu bytes = %lu frames, ok\n",
		       (unsigned long)fi.fsize, (unsigned long)(fi.fsize / 32770));
	}

	f_unmount("");
}

int main(void) {
	stdio_init_all();
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

	/* give the SD card time to power up before anything touches it */
	sleep_ms(SD_POWERUP_DELAY_MS);

	/* blink slowly until a serial terminal is opened */
	while (!stdio_usb_connected()) {
		gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
		sleep_ms(500);
	}
	sleep_ms(SD_POWERUP_DELAY_MS);   /* and again once the terminal is open */

	for (int run = 1;; run++) {
		problems = 0;
		printf("\n========== SD card test, run %d ==========\n", run);

		wire_check();
		bool sdhc;
		if (problems == 0 && card_init(&sdhc) && read_sector0())
			fatfs_check();
		else
			spi_deinit(SD_SPI);

		if (problems == 0) {
			printf("\nRESULT: ALL OK - the SD card works. Flash the main firmware.\n");
			gpio_put(PICO_DEFAULT_LED_PIN, 1);            /* LED on = ok */
		} else {
			printf("\nRESULT: %d problem(s), fix the first FAIL above first.\n", problems);
			gpio_put(PICO_DEFAULT_LED_PIN, 0);
		}
		printf("(testing again in 5 seconds; unplug USB before changing wires)\n");
		sleep_ms(5000);
	}
}
