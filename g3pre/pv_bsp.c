/* pv-vz90-dal g3pre: PV implementations of the interrupt/timer functions called by the FreeRTOS ARM926 port.
 * PV semantics (CloudpilotEmu pv_ic.cpp / pv_timer.cpp, see src/pv_hw.h): IC MASK enables lines, STATE shows
 * pending lines; timer ALARM raises IRQ 4 after the given number of 1 kHz ticks, a write to LO clears the signal.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bsp.h"
#include "interrupt.h"
#include "timer.h"
#include "FreeRTOS.h"

uint8_t ucHeap[configTOTAL_HEAP_SIZE];		/* FreeRTOS heap_4 storage (configAPPLICATION_ALLOCATED_HEAP) */

static pVectoredIsrPrototype gIsr[32];
static uint32_t gTimerLoad = 1;
volatile uint32_t gPicSpurious;

void pic_init(void)
{
	PV_IC_MASK = 0;
}

int8_t pic_registerIrq(uint8_t irq, pVectoredIsrPrototype addr, uint8_t priority)
{
	(void)priority;			/* the PV IC has no priorities */
	if (irq >= 32)
		return -1;
	gIsr[irq] = addr;
	return 0;
}

void pic_enableInterrupt(uint8_t irq)
{
	PV_IC_MASK |= 1u << irq;
}

void pic_disableInterrupt(uint8_t irq)
{
	PV_IC_MASK &= ~(1u << irq);
}

/* called from vFreeRTOS_ISR (portISR.c) with the interrupted task's context saved */
void _pic_IrqHandler(void)
{
	uint32_t pending = PV_IC_STATE & PV_IC_MASK, i;

	if (!pending)
		gPicSpurious++;
	for (i = 0; i < 32; i++)
		if ((pending & (1u << i)) && gIsr[i])
			gIsr[i]();
}

void timer_init(uint8_t timer, uint8_t counter)
{
	(void)timer; (void)counter;
	PV_TIMER_ALARM = 0;
	PV_TIMER_LO = 0;
}

void timer_setLoad(uint8_t timer, uint8_t counter, uint32_t value)
{
	(void)timer; (void)counter;
	gTimerLoad = value ? value : 1;
}

void timer_enableInterrupt(uint8_t timer, uint8_t counter)
{
	(void)timer; (void)counter;
}

void timer_start(uint8_t timer, uint8_t counter)
{
	(void)timer; (void)counter;
	PV_TIMER_ALARM = gTimerLoad;
}

/* acknowledge and re-arm (same sequence as the g1 tick handler) */
void timer_clearInterrupt(uint8_t timer, uint8_t counter)
{
	(void)timer; (void)counter;
	PV_TIMER_LO = 0;
	PV_TIMER_ALARM = gTimerLoad;
}
