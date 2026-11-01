/* pv-vz90-dal: stage g1 - minimal bring-up on the PV machine: debug output, timer, IRQ.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "pv_hw.h"

void irq_enable(void);
void probe_run(void);

static volatile uint32_t gTicksSeen;

static void dbg_putc(char c)
{
	PV_UART_DEBUG = (uint8_t)c;
}

void dbg_puts(const char *s)
{
	while (*s)
		dbg_putc(*s++);
}

void dbg_hex(uint32_t v)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	dbg_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		dbg_putc(digits[(v >> i) & 15]);
}

static uint64_t timer_now(void)
{
	uint32_t hi, lo;

	do {
		hi = PV_TIMER_HI;
		lo = PV_TIMER_LO;
	} while (PV_TIMER_HI != hi);
	return ((uint64_t)hi << 32) | lo;
}

void irq_dispatch(void)
{
	uint32_t pending = PV_IC_STATE & PV_IC_MASK;

	if (pending & (1u << PV_IRQ_TIMER)) {
		PV_TIMER_LO = 0;			/* acknowledge (pv_timer.cpp: LO write clears the alarm signal) */
		gTicksSeen++;
		PV_TIMER_ALARM = 100;		/* next alarm in 100 ms */
	}
	else {
		dbg_puts("g1: unexpected irq state ");
		dbg_hex(pending);
		dbg_puts("\n");
		PV_IC_MASK = 0;
	}
}

void exc_report(uint32_t vec, uint32_t lr)
{
	dbg_puts("g1: EXCEPTION vector ");
	dbg_hex(vec);
	dbg_puts(" lr ");
	dbg_hex(lr);
	dbg_puts("\n");
}

/* data abort report: faulting instruction = lr_abt - 8 (ARM state). For a single-register LDR/STR with an immediate
 * offset, the base register number and offset are decoded from that one instruction word, so the report says which
 * register held the bad base. The PV MPU's CP15 has no fault status/address registers (c5/c6 are MPU registers);
 * the emulator itself prints the failing address. */
void exc_report_dabt(const uint32_t *r)
{
	uint32_t pc = r[13] - 8, insn = *(const uint32_t *)pc;
	int i;

	dbg_puts("g2: DATA ABORT at ");
	dbg_hex(pc);
	dbg_puts(" insn ");
	dbg_hex(insn);
	dbg_puts(" spsr ");
	dbg_hex(r[14]);
	dbg_puts("\n");
	for (i = 0; i < 13; i++) {
		dbg_puts(i < 10 ? "g2:   r" : "g2:  r");
		dbg_hex(i);
		dbg_puts(" = ");
		dbg_hex(r[i]);
		dbg_puts("\n");
	}
	dbg_puts("g2:   sp = "); dbg_hex(r[15]); dbg_puts("  lr = "); dbg_hex(r[16]); dbg_puts("\n");
	if (((insn >> 26) & 3) == 1 && !((insn >> 25) & 1)) {
		uint32_t rn = (insn >> 16) & 15, off = insn & 0xfff;

		dbg_puts("g2:   single LDR/STR, immediate offset: base r");
		dbg_hex(rn);
		dbg_puts(" = ");
		dbg_hex(rn < 13 ? r[rn] : 0xffffffffu);
		dbg_puts(", offset ");
		dbg_puts((insn >> 23) & 1 ? "+" : "-");
		dbg_hex(off);
		dbg_puts((insn >> 20) & 1 ? " (load)\n" : " (store)\n");
	}
}

void dal_main(void)
{
	uint64_t t0;
	uint32_t last = 0;

	dbg_puts("g1: pv-vz90-dal stage g1 running from ");
	dbg_hex((uint32_t)&dal_main);
	dbg_puts("\n");

	t0 = timer_now();
	dbg_puts("g1: timer ");
	dbg_hex((uint32_t)t0);
	dbg_puts("\n");

	PV_TIMER_ALARM = 100;
	PV_IC_MASK = 1u << PV_IRQ_TIMER;
	irq_enable();

	while (last < 3) {			/* report each counted tick once, including the third (g2d display fix) */
		if (gTicksSeen != last) {
			last++;
			dbg_puts("g1: timer irq #");
			dbg_hex(last);
			dbg_puts(" at tick ");
			dbg_hex((uint32_t)(timer_now() - t0));
			dbg_puts("\n");
		}
	}
	dbg_puts("g1: done, 3 timer irqs\n");
	PV_IC_MASK = 0;
	probe_run();
}
