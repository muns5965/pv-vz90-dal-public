/* pv-vz90-dal g3pre: reset entry for the FreeRTOS test image. Same structure as src/start.S (own code);
 * SWI -> vPortYieldProcessor and IRQ -> vFreeRTOS_ISR (FreeRTOS ARM926 port). main() runs in SVC mode, as the
 * port's xPortStartScheduler() requires; tasks run in System mode. SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	b	reset
	/* g4p: Palm OS module header, so the DAL image can be found and patched as module 0 (RAL description §1):
	 * +4 export table offset, +8/+0xA L/H (ID 0), +0xC revision, +0x10 export count, +0x14 database type 'rsrc',
	 * +0x18 creator 'pdal', +0x1C resource type 'boot', +0x20 resource ID 19000, +0x22 flags (patchable | system) */
	.weak	dal_exports
	.word	dal_exports - _start
	.hword	0, 0
	.word	1
	.word	0xd0
	.word	0x72737263
	.word	0x7064616c
	.word	0x626f6f74
	.hword	19000
	.hword	0x0003
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
v_swi:		.word	swi_entry
v_pabt:		.word	exc_pabt
v_dabt:		.word	exc_dabt
v_unused:	.word	exc_unused
v_irq:		.word	vFreeRTOS_ISR
v_fiq:		.word	exc_fiq
vectors_end:

	/* g4q: SWI dispatch on the comment field (ARM: low 24 bits of the SWI word, Thumb: low 8 bits). SWI 0 is the
	 * FreeRTOS yield; any other number goes to swi_service(number, frame) (C, g4pre), which writes the result into
	 * frame[0] (r0); r1-r3, r12, lr and the caller's CPSR are restored. */
	.weak	swi_service
swi_entry:
	stmfd	sp!, {r0-r3, r12, lr}
	mrs	r12, spsr
	tst	r12, #0x20
	ldrhne	r0, [lr, #-2]
	andne	r0, r0, #0xff
	ldreq	r0, [lr, #-4]
	biceq	r0, r0, #0xff000000
	cmp	r0, #0
	ldr	r12, =swi_service
	cmpne	r12, #0
	ldmfdeq	sp!, {r0-r3, r12, lr}
	beq	vPortYieldProcessor
	mov	r1, sp
	blx	r12
	ldmfd	sp!, {r0-r3, r12, lr}
	movs	pc, lr
	.ltorg

	/* g4q: image offsets 0x100..0x1ff stay zero. In a full ROM this range of the ROM holds the storage header,
	 * which Boot reads; in the stand-alone test images (DAL at the ROM base) Boot then finds zeros, not code. */
	.org	0x100
	.space	0x100, 0
reset:
	msr	cpsr_c, #0xd2
	ldr	sp, =__stack_irq_top
	msr	cpsr_c, #0xd7
	ldr	sp, =__stack_abt_top
	msr	cpsr_c, #0xdb
	ldr	sp, =__stack_und_top
	msr	cpsr_c, #0xdf
	ldr	sp, =__stack_sys_top
	msr	cpsr_c, #0xd3
	ldr	sp, =__stack_svc_top
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
	bl	main
4:	b	4b
exc_undef:	mov	r0, #1
		b	exc_common
exc_pabt:	mov	r0, #3
		b	exc_common
exc_dabt:	mov	r0, #4
		b	exc_common
exc_unused:	mov	r0, #5
		b	exc_common
exc_fiq:	mov	r0, #7
exc_common:
	mov	r1, lr
	mrs	r2, spsr
	ldr	sp, =__stack_fatal_top
	bl	g3_exception
5:	b	5b
