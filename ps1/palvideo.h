/*
 * palvideo.h - decoder for the 16-colour video format made by
 * pc/make_video.py --mode color16. Plain C, no PS1 dependencies.
 *
 * Payload layout:
 *   [0] 'P' keyframe (start from all pixels = colour 0, new palette follows)
 *       'Q' delta    (start from the previous picture, same palette)
 *   [1] fps   [2] width (256 stored as 0)   [3] height
 *   'P' only: [4] number of colours n (1..16), then n x u16 PS1 colours (LE)
 *   then tokens, in reading order (left to right, top to bottom):
 *     0nnnnnnn          skip n+1 pixels (keep what is there)
 *     10nnnnnn  data    n+1 literal pixels, 2 per byte, first pixel in the
 *                       low nibble
 *     11nnnnnn  c       n+1 pixels of colour c (low nibble of the next byte)
 *
 * The picture is kept as colour numbers (idx[]) so a delta frame can change
 * only some pixels; pal_render() turns it into PS1 colours.
 */

#ifndef PALVIDEO_H
#define PALVIDEO_H

#include <stdint.h>

#define PAL_MAX_COLORS 16

typedef struct {
	int key;
	int fps;
	int width;
	int height;
	int data_start;           /* where the tokens begin */
} pal_info_t;

/* Read the header (and the palette of a keyframe into pal[]).
 * Returns 0 if this isn't a valid 16-colour frame. */
static inline int pal_parse_header(const uint8_t *p, int len, pal_info_t *info,
                                   uint16_t *pal) {
	if (len < 4 || (p[0] != 'P' && p[0] != 'Q'))
		return 0;
	info->key    = (p[0] == 'P');
	info->fps    = p[1];
	info->width  = p[2] ? p[2] : 256;
	info->height = p[3];
	info->data_start = 4;
	if (info->key) {
		if (len < 5)
			return 0;
		int n = p[4];
		if (n < 1 || n > PAL_MAX_COLORS || len < 5 + 2 * n)
			return 0;
		for (int i = 0; i < PAL_MAX_COLORS; i++)
			pal[i] = (i < n) ? (uint16_t)(p[5 + 2 * i] | (p[6 + 2 * i] << 8))
			                 : 0x8000;
		info->data_start = 5 + 2 * n;
	}
	return info->width > 0 && info->height > 0 && info->fps > 0;
}

/* Apply the tokens to idx[] (width*height colour numbers).
 * Returns 0 on success, -1 if the data is damaged. */
static int pal_decode(uint8_t *idx, const uint8_t *p, int len, const pal_info_t *info) {
	const int npix = info->width * info->height;
	int i = info->data_start, pos = 0;

	if (info->key)
		for (int k = 0; k < npix; k++)
			idx[k] = 0;

	while (i < len) {
		const uint8_t c = p[i++];
		const int n = (c & 0x80) ? (c & 0x3F) + 1 : (c & 0x7F) + 1;
		if (n > npix - pos)
			return -1;

		if (!(c & 0x80)) {                    /* skip */
			pos += n;
		} else if (!(c & 0x40)) {             /* literals */
			const int bytes = (n + 1) / 2;
			if (bytes > len - i)
				return -1;
			for (int k = 0; k < n; k++) {
				const uint8_t b = p[i + (k >> 1)];
				idx[pos++] = (k & 1) ? (b >> 4) : (b & 0x0F);
			}
			i += bytes;
		} else {                              /* fill */
			if (i >= len)
				return -1;
			const uint8_t col = p[i++] & 0x0F;
			for (int k = 0; k < n; k++)
				idx[pos++] = col;
		}
	}
	return 0;
}

/* Colour numbers -> PS1 colours. */
static inline void pal_render(uint16_t *out, const uint8_t *idx, int npix,
                              const uint16_t *pal) {
	for (int k = 0; k < npix; k++)
		out[k] = pal[idx[k] & 0x0F];
}

#endif
