/* pv-vz90-dal g4b: PV memory layout and HALMemoryGetMemoryMap. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_MEMORY_H
#define HAL_MEMORY_H
#include <stdint.h>
/* PV RAM layout of this DAL (32 MiB session RAM at 0x20000000):
 *   0x20000000 + 1 MiB  DAL private: data, bss, stacks, FreeRTOS heap (g3pre/link_rtos.ld RAM region)
 *   0x20100000 + 1 MiB  reserved for the framebuffer/palette (not reported to the OS)
 *   0x20200000 + 6 MiB  dynamic RAM (reported, kind 1)
 *   0x20800000 + 24 MiB storage RAM (reported, kind 2)
 * The ROM window (kind 0) is the PV ROM mapping at 0x10000000. */
#define PVMEM_RAM_REQUIRED	0x02000000u
#define PVMEM_FB_BASE		0x20100000u
#define PVMEM_DYN_BASE		0x20200000u
#define PVMEM_DYN_SIZE		0x00600000u
#define PVMEM_STOR_BASE		0x20800000u
#define PVMEM_STOR_SIZE		0x01800000u
#define PVMEM_ROM_BASE		0x10000000u
#define PVMEM_ROM_SIZE		0x02000000u
const void *hal_memory_get_memory_map(void);	/* [r9-4] slot 0x51 */
/* storage protection (g4i): TRUE = storage writable, FALSE = read-only. Logical state only: the PV MPU cannot make memory
 * read-only for privileged code and the DAL leaves it off, so writes are never blocked (docs/INTERFACE.md §22) */
uint32_t hal_memory_get_storage_protection(void);		/* 0x52 */
void hal_memory_set_storage_protection(uint32_t writable);	/* 0x53 */
uint32_t hal_memory_get_area_size(uint32_t addr);			/* 0x55: non-zero only inside the DAL's kernel heap */
#endif
