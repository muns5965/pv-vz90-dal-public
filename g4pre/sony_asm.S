/* pv-vz90-dal g4o: helpers for the Sony library replacements. SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.text

	/* uint32_t os_call(fn, r9, const uint32_t *args, n): calls an OS routine with r9 set and n word arguments
	 * (r0..r3, the rest on an 8-byte-aligned stack). Returns the callee's r0. */
	.global os_call
os_call:
	stmfd	sp!, {r4-r11, lr}
	mov	r11, sp
	mov	r12, r0
	mov	r9, r1
	mov	r4, r2
	subs	r5, r3, #4		/* stack words */
	movlt	r5, #0
	sub	sp, sp, r5, lsl #2
	bic	sp, sp, #7
	mov	r6, #0
1:	cmp	r6, r5
	bge	2f
	add	r7, r6, #4
	ldr	r8, [r4, r7, lsl #2]
	str	r8, [sp, r6, lsl #2]
	add	r6, r6, #1
	b	1b
2:	cmp	r3, #0
	ldrgt	r0, [r4, #0]
	cmp	r3, #1
	ldrgt	r1, [r4, #4]
	cmp	r3, #2
	ldrgt	r2, [r4, #8]
	cmp	r3, #3
	ldrgt	r3, [r4, #12]
	blx	r12
	mov	sp, r11
	ldmfd	sp!, {r4-r11, pc}

	/* halting replacement entries: r12 = (module << 8) | index, then sony_halt(code, lr, r0, r1) */
	.macro HSTUBS mod, n
	.set i, 0
	.rept \n
	mov	r12, #(\mod << 8)
	orr	r12, r12, #i
	b	halt_entry
	.set i, i + 1
	.endr
	.endm

halt_entry:
	mov	r3, r1
	mov	r2, r0
	mov	r1, lr
	mov	r0, r12
	bl	sony_halt		/* does not return */
3:	b	3b

	.global sony_halt_sdal, sony_halt_rm, sony_halt_p1, sony_halt_ge, sony_halt_i2
	.align 2
sony_halt_sdal:	HSTUBS 1, 45		/* SonyDAL entries that halt */
sony_halt_rm:	HSTUBS 2, 64		/* RmcExt: every entry */
sony_halt_p1:	HSTUBS 3, 64		/* Pa1Lib: every entry */
sony_halt_ge:	HSTUBS 4, 22		/* GE2D entries 0..21 */
sony_halt_i2:	HSTUBS 5, 1		/* main-entry commands other than load/unload */
