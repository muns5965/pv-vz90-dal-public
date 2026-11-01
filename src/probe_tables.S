/* pv-vz90-dal stage g2a probe: three dispatch tables (accessed by modules as [r9-4], [r9-8], [r9-12])
 * whose every slot points to a stub that records (table, slot) and stops. Used to observe which interface a
 * Palm OS module calls first. SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.text
	.macro STUBS tab, n		/* g4o: 8-byte stubs; the table and slot are derived from the stub address */
	.rept \n
	mov	r12, pc
	b	probe_trap_entry
	.endr
	.endm
	.macro TABLE tab, n
	.set i, 0
	.rept \n
	.word	stubs_\tab + 8 * i
	.set i, i + 1
	.endr
	.endm

	.align 4
stubs_1:	STUBS 1, 256		/* DAL table: capacity 0x100 (RAL description §3) */
stubs_2:	STUBS 2, 1024
stubs_3:	STUBS 3, 384		/* UI table: capacity 0x180 */

	/* g4p: the DAL's export table (module header +4): one branch per DAL slot to its stub */
	.global dal_exports
	.align 2
dal_exports:
	.set i, 0
	.rept 0xd0
	b	stubs_1 + 8 * i
	.set i, i + 1
	.endr

	.section .rodata
	.align 4
	.global probe_tab1, probe_tab2, probe_tab3
probe_tab1:	TABLE 1, 256
probe_tab2:	TABLE 2, 1024
probe_tab3:	TABLE 3, 384

	.text
probe_trap_entry:
	/* r0-r3 = the call's arguments, r12 = (table << 12) | slot, lr = caller return address
	 * frame for probe_trap(): args[0] = CPSR at the call, args[1..4] = r0..r3, args[5] = lr; caller sp = &args[6].
	 * probe_trap() returns 0 = stop here, 1 = return to the caller with r0..r3 taken from the frame (g2e replay, g4pre impl). */
	stmfd	sp!, {r0-r3, lr}
	sub	r12, r12, #8		/* r12 = stub address (mov r12, pc reads stub + 8) */
	ldr	r0, =stubs_2
	ldr	r1, =stubs_3
	mov	r2, #(1 << 12)
	cmp	r12, r0
	ldrlo	r0, =stubs_1		/* table 1 */
	movhs	r2, #(2 << 12)
	cmp	r12, r1
	movhs	r0, r1			/* table 3 */
	movhs	r2, #(3 << 12)
	sub	r12, r12, r0
	orr	r12, r2, r12, lsr #3	/* (table << 12) | slot */
	mrs	r0, cpsr
	stmfd	sp!, {r0}
	mov	r0, r12
	mov	r1, lr
	mov	r2, sp
	mov	r3, r9
	mov	r12, sp			/* g4o: OS callers do not keep sp 8-byte aligned; the C handlers need it */
	bic	sp, sp, #7
	sub	sp, sp, #8
	str	r12, [sp]
	bl	probe_trap
	ldr	sp, [sp]
	cmp	r0, #0
1:	beq	1b
	add	sp, sp, #4
	ldmfd	sp!, {r0-r3, lr}	/* r0..r3 as left in the frame by probe_trap() (replay: 0, implemented: result) */
	bx	lr

	.global probe_call_module
probe_call_module:
	/* r0 = entry, r1 = r9 value, r2/r3 = r0/r1 for the module, [sp] = r3 for the module, [sp+4] = CPSR to enter with
	 * (mode System; flags as observed per call, stage g2e). Runs the module on its own stack (__stack_sys_top,
	 * 32 KiB) and returns in SVC mode with the SVC stack untouched. */
	stmfd	sp!, {r4-r11, lr}
	ldr	r5, [sp, #36]
	ldr	r4, [sp, #40]
	mov	r9, r1
	mov	r12, r0
	mov	r0, r2
	mov	r1, r3
	mov	r2, #0
	mov	r3, r5
	msr	cpsr_fc, r4
	ldr	sp, =__stack_sys_top
	blx	r12
	msr	cpsr_c, #0xd3		/* back to SVC: SVC sp/lr are banked and still point at the frame above */
	ldmfd	sp!, {r4-r11, pc}

	.global probe_call_in_task
probe_call_in_task:
	/* stage g4-pre: call a module entry from a FreeRTOS task (System mode, task stack, IRQs enabled).
	 * r0 = entry, r1 = r9 value, r2 = CPSR condition flags (bits 31..28) to enter with; module gets r0..r3 = 0. */
	stmfd	sp!, {r4-r11, lr}
	mov	r9, r1
	mov	r12, r0
	ldr	r3, =gProbeEntrySp
	str	sp, [r3]
	msr	cpsr_f, r2
	mov	r0, #0
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	blx	r12
	ldmfd	sp!, {r4-r11, pc}
	.ltorg

	.global probe_call_with_r9
probe_call_with_r9:
	/* stage g4j: call module code (an OS callback) with its own r9: r0 = function, r1 = r9, r2/r3 = first two
	 * arguments for the callee, r2 = 0. Returns the callee's r0. */
	stmfd	sp!, {r4-r11, lr}
	mov	r12, r0
	mov	r9, r1
	mov	r0, r2
	mov	r1, r3
	mov	r2, #0
	blx	r12
	ldmfd	sp!, {r4-r11, pc}

	.global ral_call_new_stack
ral_call_new_stack:
	/* stage g4k, RALCallWithNewStack: r0 = function, r1 = parameter, r2 = new stack top, r3 = caller's r9. Calls
	 * function(parameter) on the new stack (8-byte aligned below the top) with that r9, restores sp, returns the
	 * callee's r0/r1 unchanged. */
	stmfd	sp!, {r4, r9, lr}
	mov	r9, r3
	mov	r4, sp
	bic	sp, r2, #7
	mov	r12, r0
	mov	r0, r1
	blx	r12
	mov	sp, r4
	ldmfd	sp!, {r4, r9, pc}

	.weak	ral_serial_result, gRalSerialOrig	/* defined by g4pre/ral.c; absent (never called) in the g1 image */
	.global ral_serial_install_wrap
ral_serial_install_wrap:
	/* stage g4k: Boot entry 0x19b (SerialMgrInstall) replacement: run the original (same r9, no arguments), then let
	 * ral_serial_result() map the result (C, preserves r9) */
	stmfd	sp!, {r4, lr}
	ldr	r12, =gRalSerialOrig
	ldr	r12, [r12]
	blx	r12
	bl	ral_serial_result
	ldmfd	sp!, {r4, pc}
	.ltorg
