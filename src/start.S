/* pv-vz90-dal: reset entry and exception vectors. Written from the ARMv5 architecture rules (vector layout,
 * CPSR modes, CP15 c1 V bit) and the PV memory map in pv_hw.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	b	reset

/* table copied to the PV high-vector RAM (0xFFFF0000); "ldr pc" form so it works from any address */
	.global vectors_begin
vectors_begin:
	ldr	pc, v_reset
	ldr	pc, v_undef
	ldr	pc, v_swi
	ldr	pc, v_pabt
	ldr	pc, v_dabt
	ldr	pc, v_unused
	ldr	pc, v_irq
	ldr	pc, v_fiq
v_reset:	.word	reset
v_undef:	.word	exc_undef
v_swi:		.word	exc_swi
v_pabt:		.word	exc_pabt
v_dabt:		.word	exc_dabt
v_unused:	.word	exc_unused
v_irq:		.word	irq_entry
v_fiq:		.word	exc_fiq
	.global vectors_end
vectors_end:

#define MODE_IRQ	0x12
#define MODE_SVC	0x13
#define MODE_ABT	0x17
#define MODE_UND	0x1b
#define I_F_BITS	0xc0

reset:
	/* stacks: each mode gets its own area at the top of the stack region (linker symbols) */
	msr	cpsr_c, #(MODE_IRQ | I_F_BITS)
	ldr	sp, =__stack_irq_top
	msr	cpsr_c, #(MODE_ABT | I_F_BITS)
	ldr	sp, =__stack_abt_top
	msr	cpsr_c, #(MODE_UND | I_F_BITS)
	ldr	sp, =__stack_und_top
	msr	cpsr_c, #(MODE_SVC | I_F_BITS)
	ldr	sp, =__stack_svc_top

	/* .data from ROM to RAM, clear .bss */
	ldr	r0, =__data_load
	ldr	r1, =__data_start
	ldr	r2, =__data_end
1:	cmp	r1, r2
	ldrlo	r3, [r0], #4
	strlo	r3, [r1], #4
	blo	1b
	ldr	r1, =__bss_start
	ldr	r2, =__bss_end
	mov	r3, #0
2:	cmp	r1, r2
	strlo	r3, [r1], #4
	blo	2b

	/* vectors to 0xFFFF0000 and select high vectors (CP15 c1 bit 13) */
	ldr	r0, =vectors_begin
	ldr	r1, =vectors_end
	ldr	r2, =0xffff0000
3:	cmp	r0, r1
	ldrlo	r3, [r0], #4
	strlo	r3, [r2], #4
	blo	3b
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #0x2000
	mcr	p15, 0, r0, c1, c0, 0

	bl	dal_main
4:	b	4b

/* IRQs are not nested: the handler runs with IRQs masked (CPSR.I set on entry) and returns with the
 * pre-exception CPSR restored from SPSR. */
irq_entry:
	sub	lr, lr, #4
	stmfd	sp!, {r0-r3, r12, lr}
	bl	irq_dispatch
	ldmfd	sp!, {r0-r3, r12, pc}^

/* fatal exceptions: report vector number and the faulting address in lr */
exc_undef:	mov	r0, #1
		b	exc_common
exc_swi:	mov	r0, #2
		b	exc_common
exc_pabt:	mov	r0, #3
		b	exc_common
exc_dabt:
	/* full state for data aborts: r0-r12, lr_abt, spsr, and sp/lr of the interrupted mode */
	ldr	sp, =exc_regs
	stmia	sp, {r0-r12}
	str	lr, [sp, #13*4]
	mrs	r0, spsr
	str	r0, [sp, #14*4]
	mrs	r1, cpsr
	and	r2, r0, #0x1f
	cmp	r2, #0x10		/* user mode: use System mode to reach the shared sp/lr */
	moveq	r2, #0x1f
	bic	r3, r1, #0x1f
	orr	r3, r3, r2
	orr	r3, r3, #0xc0
	msr	cpsr_c, r3
	mov	r4, sp
	mov	r5, lr
	msr	cpsr_c, r1
	ldr	sp, =exc_regs
	str	r4, [sp, #15*4]
	str	r5, [sp, #16*4]
	ldr	sp, =__stack_fatal_top
	ldr	r0, =exc_regs
	bl	exc_report_dabt
6:	b	6b
exc_unused:	mov	r0, #5
		b	exc_common
exc_fiq:	mov	r0, #7
exc_common:
	mov	r1, lr
	ldr	sp, =__stack_fatal_top
	bl	exc_report
5:	b	5b

	.bss
	.align 2
	.global exc_regs
exc_regs:	.space 17*4		/* r0-r12, lr_abt, spsr, sp, lr of the aborted mode */
	.text

	.global irq_enable
irq_enable:
	mrs	r0, cpsr
	bic	r0, r0, #0x80
	msr	cpsr_c, r0
	bx	lr
