/* pv-vz90-dal g4b: HALMemoryGetMemoryMap. Requirements: local functional spec (analyst role) and reference
 * observation (docs/INTERFACE.md §12). Contract: no arguments, returns a never-NULL pointer to the same read-only
 * record on every call, no side effects.
 * Record (8 bytes): +0x00 u8 entry count, +0x01..+0x03 zero, +0x04 pointer to the entry array.
 * Entry (12 bytes): +0x00 u8 kind (0 ROM, 1 dynamic RAM, 2 storage RAM), +0x01..+0x03 zero, +0x04 base, +0x08 size.
 * Boot finds entries by kind (it reads the kind byte of every entry while searching); order kept ROM/dyn/storage.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_memory.h"
#include "FreeRTOS.h"

struct PvMemRegion {
	uint8_t kind;
	uint8_t pad[3];
	uint32_t base;
	uint32_t size;
};

struct PvMemMap {
	uint8_t count;
	uint8_t pad[3];
	const struct PvMemRegion *regions;
};

_Static_assert(sizeof(struct PvMemRegion) == 12, "entry must be 12 bytes");
_Static_assert(sizeof(struct PvMemMap) == 8, "record must be 8 bytes");

static const struct PvMemRegion gPvRegions[3] = {
	{ 0, { 0, 0, 0 }, PVMEM_ROM_BASE, PVMEM_ROM_SIZE },
	{ 1, { 0, 0, 0 }, PVMEM_DYN_BASE, PVMEM_DYN_SIZE },
	{ 2, { 0, 0, 0 }, PVMEM_STOR_BASE, PVMEM_STOR_SIZE },
};

static const struct PvMemMap gPvMemMap = { 3, { 0, 0, 0 }, gPvRegions };

const void *hal_memory_get_memory_map(void)
{
	return &gPvMemMap;
}

/* storage protection: recorded state, read-only (FALSE) until the OS first sets it */
static uint32_t gStorageWritable;

uint32_t hal_memory_get_storage_protection(void)
{
	return gStorageWritable;
}

void hal_memory_set_storage_protection(uint32_t writable)
{
	gStorageWritable = (writable & 0xff) ? 1 : 0;
}

/* the DAL's kernel heap is the FreeRTOS heap array (heap_4.c) */
extern uint8_t ucHeap[configTOTAL_HEAP_SIZE];

uint32_t hal_memory_get_area_size(uint32_t addr)
{
	uint32_t lo = (uint32_t)ucHeap;

	return (addr >= lo && addr < lo + configTOTAL_HEAP_SIZE) ? 1024 : 0;
}
