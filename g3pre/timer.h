/* pv-vz90-dal g3pre: tick-timer API used by the FreeRTOS ARM926 port, implemented for the PV 1 kHz timer.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TIMER_H
#define TIMER_H
#include <stdint.h>
void timer_init(uint8_t timer, uint8_t counter);
void timer_setLoad(uint8_t timer, uint8_t counter, uint32_t value);
void timer_enableInterrupt(uint8_t timer, uint8_t counter);
void timer_start(uint8_t timer, uint8_t counter);
void timer_clearInterrupt(uint8_t timer, uint8_t counter);
#endif
