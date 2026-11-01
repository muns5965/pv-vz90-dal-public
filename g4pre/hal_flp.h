/* pv-vz90-dal g4n: HALFlp* slots. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_FLP_H
#define HAL_FLP_H
#include <stdint.h>
int hal_flp_dispatch(uint32_t slot, uint32_t *args);	/* 1 = handled, result in args[1] (and args[2]) */
#endif
