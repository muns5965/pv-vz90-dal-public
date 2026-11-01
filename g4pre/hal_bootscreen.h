/* pv-vz90-dal g4s: HALDisplayDrawBootScreen. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_BOOTSCREEN_H
#define HAL_BOOTSCREEN_H
#include <stdint.h>
uint32_t hal_display_draw_boot_screen(int32_t x, int32_t y, const uint8_t *bmp);	/* slot 0x84, always 0 */
#endif
