/* pv-vz90-dal g4r/g4s: drawing and the boot screen left out of the 96 KiB Boot-only test image (the Boot-only run never reaches the UI).
 * The full-ROM DAL links hal_draw.c instead. SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_draw.h"
#include "hal_bootscreen.h"

uint32_t hal_draw_bitmap(const uint8_t *c, const uint8_t *s, const int16_t *r, int32_t x, int32_t y)
{ (void)c; (void)s; (void)r; (void)x; (void)y; return 0; }
uint32_t hal_draw_get_pixel(const uint8_t *b, int32_t x, int32_t y, uint32_t i)
{ (void)b; (void)x; (void)y; (void)i; return 0; }
void hal_draw_chars(const uint8_t *c, int32_t x, int32_t y, const uint8_t *s, int32_t n, const uint8_t *f,
		    const uint8_t *m, uint32_t cb, uint32_t r9)
{ (void)c; (void)x; (void)y; (void)s; (void)n; (void)f; (void)m; (void)cb; (void)r9; }
void hal_draw_line(const uint8_t *c, int32_t a, int32_t b, int32_t d, int32_t e, int32_t w)
{ (void)c; (void)a; (void)b; (void)d; (void)e; (void)w; }
void hal_draw_rectangle(const uint8_t *c, const int16_t *r, int32_t a, int32_t w) { (void)c; (void)r; (void)a; (void)w; }
void hal_draw_pixels(const uint8_t *c, uint32_t n, const int16_t *p, int32_t w) { (void)c; (void)n; (void)p; (void)w; }
uint32_t hal_display_draw_boot_screen(int32_t x, int32_t y, const uint8_t *b) { (void)x; (void)y; (void)b; return 0; }
