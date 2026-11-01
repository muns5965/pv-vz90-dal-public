/* pv-vz90-dal full-ROM DAL linker script, derived from the g3pre script (src/link.ld + 4-byte aligned end of .text so the .data load address is aligned). SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)
MEMORY {
	ROM (rx)  : ORIGIN = 0x10000230, LENGTH = 0x40000		/* full-ROM build: DAL code runs in place as the first ROM heap chunk */
	RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x100000	/* stage-g1 test area only; the Palm OS memory layout is defined later */
}
SECTIONS {
	.text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) . = ALIGN(4); } > ROM
	.data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > ROM
	__data_load = LOADADDR(.data);
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
	. = ALIGN(8);
	. = . + 0x400;  __stack_fatal_top = .;
	. = . + 0x400;  __stack_abt_top = .;
	. = . + 0x400;  __stack_und_top = .;
	. = . + 0x1000; __stack_irq_top = .;
	. = . + 0x4000; __stack_svc_top = .;
	. = . + 0x8000; __stack_sys_top = .;	/* System mode stack for module calls (separate from SVC) */
	__stack_sys_bottom = __stack_sys_top - 0x8000;
}
