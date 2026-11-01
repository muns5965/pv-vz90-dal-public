/* pv-vz90-dal g3pre: register-preservation task. Holds r0..r12 = 1..13 and counts in lr while being preempted by
 * the tick IRQ and yielding itself through SWI (taskYIELD); any changed register is reported in gRegFail
 * (1..13 = r0..r12 changed, 14 = lr counter went backwards). SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.text
	.global reg_check_task
reg_check_task:
	mov	r0, #1
	mov	r1, #2
	mov	r2, #3
	mov	r3, #4
	mov	r4, #5
	mov	r5, #6
	mov	r6, #7
	mov	r7, #8
	mov	r8, #9
	mov	r9, #10
	mov	r10, #11
	mov	r11, #12
	mov	r12, #13
	mov	lr, #0
loop:
	cmp	r0, #1
	bne	f0
	cmp	r1, #2
	bne	f1
	cmp	r2, #3
	bne	f2
	cmp	r3, #4
	bne	f3
	cmp	r4, #5
	bne	f4
	cmp	r5, #6
	bne	f5
	cmp	r6, #7
	bne	f6
	cmp	r7, #8
	bne	f7
	cmp	r8, #9
	bne	f8
	cmp	r9, #10
	bne	f9
	cmp	r10, #11
	bne	f10
	cmp	r11, #12
	bne	f11
	cmp	r12, #13
	bne	f12
	add	lr, lr, #1
	ldr	r12, =gRegIters
	ldr	r0, [r12]
	cmp	lr, r0
	bls	f13			/* lr must keep increasing */
	str	lr, [r12]
	mov	r0, #1
	mov	r12, #13
	tst	lr, #0xff00
	tsteq	lr, #0xff
	bne	loop
	/* every 0x10000 iterations: yield through SWI (vPortYieldProcessor) */
	ldr	r12, =gRegYields
	ldr	r0, [r12]
	add	r0, r0, #1
	str	r0, [r12]
	mov	r0, #1
	mov	r12, #13
	swi	0
	b	loop
f0:	mov	r0, #1
	b	fail
f1:	mov	r0, #2
	b	fail
f2:	mov	r0, #3
	b	fail
f3:	mov	r0, #4
	b	fail
f4:	mov	r0, #5
	b	fail
f5:	mov	r0, #6
	b	fail
f6:	mov	r0, #7
	b	fail
f7:	mov	r0, #8
	b	fail
f8:	mov	r0, #9
	b	fail
f9:	mov	r0, #10
	b	fail
f10:	mov	r0, #11
	b	fail
f11:	mov	r0, #12
	b	fail
f12:	mov	r0, #13
	b	fail
f13:	mov	r0, #14
fail:	ldr	r12, =gRegFail
	str	r0, [r12]
1:	b	1b
	.ltorg
