/* pv-vz90-dal g4f: random-number and time HAL. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_TIME_H
#define HAL_TIME_H
#include <stdint.h>
void hal_random_init(void);
uint32_t hal_random_get_bits(uint32_t numBits, void *bitsP);		/* [r9-4] 0x61 */
uint32_t hal_random_initialize_seed(uint32_t *randomSeedP);		/* [r9-4] 0x62: returns a seed value */
uint32_t hal_time_get_seconds(void);							/* 0x73 */
uint32_t hal_time_set_seconds(uint32_t seconds);				/* 0x77 */
uint32_t hal_time_get_system_time(void);						/* 0x74: ms since power-on */
uint32_t hal_delay(uint32_t microseconds);						/* 0x0a */
#endif
