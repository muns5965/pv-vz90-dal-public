/* pv-vz90-dal: PV (CloudpilotEmu "rePalm paravirtualized") device register map.
 * Source of every value: CloudpilotEmu (GPLv3) src/uarm/uarm/{soc_pv,pv_ic,pv_uart,pv_timer}.cpp, commit 5e3a22e2.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PV_HW_H
#define PV_HW_H

#include <stdint.h>

#define PV_ROM_BASE		0x10000000u	/* soc_pv.cpp ROM_BASE, reset PC */
#define PV_RAM_BASE		0x20000000u	/* soc_pv.cpp RAM_BASE */
#define PV_TINYRAM_BASE	0xffff0000u	/* soc_pv.cpp PV_TINYRAM_BASE (high vectors live here) */
#define PV_TINYRAM_SIZE	0x00000100u

#define PV_REG32(a)		(*(volatile uint32_t *)(a))

/* interrupt controller (pv_ic.cpp): MASK r/w, STATE read-only; CPU IRQ line = (STATE & MASK) != 0 */
#define PV_IC_MASK		PV_REG32(0x30000000u)
#define PV_IC_STATE		PV_REG32(0x30000004u)

/* debug UART (pv_uart.cpp, soc_pv.cpp UART_DEBUG_BASE): 32-bit write sends the low byte to the host stderr */
#define PV_UART_DEBUG	PV_REG32(0x30000100u)

/* timer (pv_timer.cpp): 64-bit tick counter at 1 kHz (LO/HI read), writing LO acknowledges the alarm IRQ,
 * ALARM = ticks until the alarm fires (0 = off), IRQ number 4 */
#define PV_TIMER_LO		PV_REG32(0x30000600u)
#define PV_TIMER_HI		PV_REG32(0x30000604u)
#define PV_TIMER_ALARM	PV_REG32(0x30000608u)
#define PV_IRQ_TIMER	4u
#define PV_TIMER_HZ		1000u

/* display (pv_display.cpp): BASE/STRIDE (4-byte aligned), DEPTH 0..4 = 1/2/4/8/16 bpp, CLUT write = (index << 24) | RGB888,
 * SPEC read-only: bits 0-8 = width/4 - 1, bits 9-17 = height/4 - 1, bits 18-20 = density/36 - 1 (72 dpi -> 1, 144 -> 3) */
#define PV_DISP_BASE	PV_REG32(0x30000200u)
#define PV_DISP_STRIDE	PV_REG32(0x30000204u)
#define PV_DISP_DEPTH	PV_REG32(0x30000208u)
#define PV_DISP_CLUT	PV_REG32(0x3000020cu)
#define PV_DISP_SPEC	PV_REG32(0x30000210u)

/* keys (pv_keys.cpp): +0 current key word (read), any write clears the key IRQ; IRQ 3 on every change.
 * Bits: power 0x1, page up 0x2, page down 0x4, hard1..4 0x8..0x40, left 0x01000000, right 0x02000000, select 0x08000000 */
#define PV_KEYS_STATE	PV_REG32(0x30000500u)
#define PV_IRQ_KEYS		3u
/* touch (pv_touch.cpp): +0 read = x (bits 0-15) | y (bits 16-31), reading clears the touch IRQ; IRQ 1 on every change;
 * pen up is reported as x = y = -1 (SoC.cpp PenUp) */
#define PV_TOUCH_STATE	PV_REG32(0x30000300u)
#define PV_IRQ_TOUCH	1u

/* RTC (pv_rtc.cpp): +0 read = host time in Palm OS epoch seconds (since 1904), +4 write clears the 1 Hz IRQ 2.
 * Timer LO/HI is a free-running 1 kHz counter; a write to LO only clears the alarm signal (pv_timer.cpp). */
#define PV_RTC_SECONDS	PV_REG32(0x30000400u)
#define PV_RTC_CLEARIRQ	PV_REG32(0x30000404u)
#define PV_IRQ_RTC		2u

#define PV_SYSCTL_RAMSIZE	PV_REG32(0x30000900u)	/* pv_sysctl.cpp: offset 0 = RAM size in bytes (read-only) */

#endif
