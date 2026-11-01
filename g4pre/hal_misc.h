/* pv-vz90-dal g4j: miscellaneous HAL services (identity, tokens, constants, power, interrupts, debug, sound stubs).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_MISC_H
#define HAL_MISC_H
#include <stdint.h>
/* handles [r9-4] slot `slot` with the trap frame args[1..4] = r0..r3 (result to args[1]); returns 1 if handled,
 * 0 if the slot is not one of these services (or must stop, e.g. HALReset) */
int hal_misc_dispatch(uint32_t slot, uint32_t *args, uint32_t r9);
#endif
