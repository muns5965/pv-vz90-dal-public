/* pv-vz90-dal g4r: HALDraw rendering (16 bpp destinations). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_DRAW_H
#define HAL_DRAW_H
#include <stdint.h>
uint32_t hal_draw_bitmap(const uint8_t *canvas, const uint8_t *srcBmp, const int16_t *rect, int32_t xOff, int32_t yOff); /* 0xbe */
uint32_t hal_draw_get_pixel(const uint8_t *bmp, int32_t x, int32_t y, uint32_t asIndex);	/* 0xc0 */
void hal_draw_chars(const uint8_t *canvas, int32_t x, int32_t y, const uint8_t *chars, int32_t len, const uint8_t *fontP,
		    const uint8_t *fontMap, uint32_t checkCb, uint32_t r9);			/* 0xc1 */
void hal_draw_line(const uint8_t *canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t penW);	/* 0xc2 */
void hal_draw_rectangle(const uint8_t *canvas, const int16_t *rect, int32_t radius, int32_t penW);	/* 0xc3 */
void hal_draw_pixels(const uint8_t *canvas, uint32_t n, const int16_t *pts, int32_t penW);		/* 0xc4 */
uint32_t hal_draw_find_indexes(uint32_t count, uint8_t *recs, const void *tableP);			/* hal_display.c */
void hal_draw_std_colour(uint32_t depth, uint32_t index, uint8_t *rec);	/* RGBColorType of a standard index */
#endif
