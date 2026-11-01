/* pv-vz90-dal g4d: PV display HAL (early-boot subset). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H
#include <stdint.h>
#define HALDISP_ERR_UNKNOWN_ATTR	0x2201u
#define HALDISP_ERR_REFUSED			0xffffu
void hal_display_init(void);
/* return 1 when the attribute is handled (result in *err), 0 for an attribute this DAL does not implement yet
 * (the caller then stops at the trap instead of guessing) */
int hal_display_get_attr(uint32_t attr, void *valueP, uint32_t *err);
int hal_display_set_attr(uint32_t attr, uint32_t value, uint32_t *err);
uint32_t hal_display_set_palette(int32_t start, uint32_t count, const void *entries);
/* g4l: screen bitmap group (slots 0x64, 0x66..0x69, 0x6b, 0x6c) */
uint32_t hal_screen_init(uint32_t *outP, const void *paletteP);
uint32_t hal_screen_update_bitmap(uint32_t depth);
uint32_t hal_screen_get_colortable(void);
uint32_t hal_screen_palette(int32_t start, uint32_t count, uint32_t depth, const void *tableP, const void *const *palettes);
uint32_t hal_screen_default_palette(void);
uint32_t hal_screen_lock(uint32_t mode, int *fatal);
uint32_t hal_screen_unlock(void);
void hal_display_geometry(uint32_t *w, uint32_t *h, uint32_t *depth);
uint32_t hal_draw_find_indexes(uint32_t count, uint8_t *recs, const void *tableP);	/* 0x16 / 0xbf */
#endif
