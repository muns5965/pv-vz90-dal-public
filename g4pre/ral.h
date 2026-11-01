/* pv-vz90-dal g4k: RAL module loader. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RAL_H
#define RAL_H
#include <stdint.h>
int ral_init(void);					/* after probe_prepare(): records DAL and Boot, Boot patches */
void ral_memory_stage(void);				/* HALSetInitStage(1) */
int ral_dispatch(uint32_t slot, uint32_t *args, uint32_t r9);	/* [r9-4] 0xad..0xb6; 0 = not handled or stop */
#endif
