/*
 * bwvideo.h - decoder for the black & white video format made by
 * pc/make_video.py --mode bw. Plain C, no PS1 dependencies, so it can be
 * tested on a PC as well.
 *
 * Payload layout:
 *   [0] 'K' keyframe (start from black) or 'D' delta (start from previous)
 *   [1] fps   [2] width   [3] height
 *   [4..] varint run lengths, alternating keep / flip, starting with keep
 *
 * Pixels are PS1 15-bit colours with bit 15 set, because in a 15-bit
 * texture the value 0x0000 means "transparent", not black:
 *   black = 0x8000, white = 0xFFFF, flipping = XOR 0x7FFF
 */

#ifndef BWVIDEO_H
#define BWVIDEO_H

#include <stdint.h>

#define BW_BLACK 0x8000u
#define BW_WHITE 0xFFFFu
#define BW_FLIP  0x7FFFu

#define BW_HEADER 4

typedef struct {
	int key;      /* 1 = keyframe        */
	int fps;
	int width;
	int height;
} bw_info_t;

/* Read the 4-byte header. Returns 0 if this isn't a bw frame. */
static inline int bw_parse_header(const uint8_t *p, int len, bw_info_t *info) {
	if (len < BW_HEADER || (p[0] != 'K' && p[0] != 'D'))
		return 0;
	info->key    = (p[0] == 'K');
	info->fps    = p[1];
	info->width  = p[2] ? p[2] : 256;   /* 256 doesn't fit a byte: stored as 0 */
	info->height = p[3];
	return info->width > 0 && info->height > 0 && info->fps > 0;
}

/* Apply one frame to the picture in fb (width*height pixels).
 * Returns 0 on success, -1 if the data is damaged (picture may be partly
 * updated; the next keyframe repairs it). */
static int bw_decode(uint16_t *fb, const uint8_t *p, int len, const bw_info_t *info) {
	const int npix = info->width * info->height;
	int i = BW_HEADER, pos = 0, flip = 0;

	if (info->key)
		for (int k = 0; k < npix; k++)
			fb[k] = BW_BLACK;

	while (i < len) {
		uint32_t run = 0;
		int shift = 0;
		uint8_t b;
		do {
			if (i >= len || shift > 21)
				return -1;
			b = p[i++];
			run |= (uint32_t)(b & 0x7F) << shift;
			shift += 7;
		} while (b & 0x80);

		if (run > (uint32_t)(npix - pos))
			return -1;

		if (flip) {
			uint16_t *px = fb + pos;
			for (uint32_t k = 0; k < run; k++)
				px[k] ^= BW_FLIP;
		}
		pos += (int)run;
		flip ^= 1;
	}
	return 0;
}

#endif
