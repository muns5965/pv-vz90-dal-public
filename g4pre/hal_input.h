/* pv-vz90-dal g4e: key input HAL (early-boot subset). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_INPUT_H
#define HAL_INPUT_H
#include <stdint.h>
uint32_t hal_key_get_state(void);				/* [r9-4] 0x4d */
uint32_t hal_key_set_mask(uint32_t mask);		/* [r9-4] 0x4f, returns the previous mask */
uint32_t hal_key_get_rates(uint16_t *initDelayP, uint16_t *periodP, uint8_t *queueAheadP);	/* 0x4c */
uint32_t hal_key_set_rates(uint16_t initDelay, uint16_t period, uint8_t queueAhead);		/* 0x50 */
/* g4q: pen calibration, [r9-4] 0x57 / 0x5a / 0x56 / 0x58 */
uint32_t hal_pen_raw_to_screen(int16_t *pt);
uint32_t hal_pen_screen_to_raw(int16_t *pt);
uint32_t hal_pen_calibrate(const int16_t *digTL, const int16_t *digBR, const int16_t *scrTL, const int16_t *scrBR,
			   uint32_t density);
uint32_t hal_pen_reset_calibration(void);
uint32_t hal_key_get_mask(void);			/* g4r: current HALKeySetMask mask */
#endif
