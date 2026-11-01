/* pv-vz90-dal linker script. SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)
MEMORY {
	ROM (rx)  : ORIGIN = 0x10000000, LENGTH = 0x18000		/* code (+ probe tables); ROM card header is placed at +0x18000 by tools/mkimg.py */
	RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x100000	/* stage-g1 test area only; the Palm OS memory layout is defined later */
}
SECTIONS {
	.text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > ROM
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
