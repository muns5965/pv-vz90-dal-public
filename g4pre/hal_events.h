/* pv-vz90-dal g4j: init stages, driver events and OS event callbacks. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_EVENTS_H
#define HAL_EVENTS_H
#include <stdint.h>
uint32_t hal_set_init_stage(uint32_t stage);						/* [r9-4] 0x6d */
uint32_t hal_event_register_callback(uint32_t type, uint32_t cb, uint32_t *oldP, uint32_t r9);	/* 0x1f */
uint32_t hal_event_post(uint32_t type, const void *data);			/* 0x1e */
int hal_input_driver_up(void);
uint32_t hal_input_tracked_keys(void);
uint32_t hal_get_init_stage(void);			/* 0xffffffff before the first HALSetInitStage */
uint32_t hal_time_get_alarm(uint32_t *secondsP);		/* 0x72 */
uint32_t hal_time_set_alarm(uint32_t seconds);			/* 0x76 */
#endif
