/* pv-vz90-dal g3pre: interrupt-controller API used by the FreeRTOS ARM926 port, implemented for the PV IC
 * (mask/state registers, pv_hw.h). New code; same function names as the port expects.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INTERRUPT_H
#define INTERRUPT_H
#include <stdint.h>
#define PIC_MAX_PRIORITY	127
typedef void (*pVectoredIsrPrototype)(void);
void pic_init(void);
int8_t pic_registerIrq(uint8_t irq, pVectoredIsrPrototype addr, uint8_t priority);
void pic_enableInterrupt(uint8_t irq);
void pic_disableInterrupt(uint8_t irq);
void _pic_IrqHandler(void);
#endif
