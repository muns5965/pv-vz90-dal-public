/* pv-vz90-dal g3pre: board definitions the FreeRTOS ARM926 port (port.c) expects, for the PV platform.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BSP_H
#define BSP_H
#include "../src/pv_hw.h"
#define BSP_NR_TIMERS	1
#define BSP_TIMER_IRQS	{ PV_IRQ_TIMER }
#endif
