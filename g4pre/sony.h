/* pv-vz90-dal g4o: Sony VZ90 library replacements and stage hook. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SONY_H
#define SONY_H
#include <stdint.h>
int sony_condition(void);			/* init stage >= 1 and a SonyDAL database is present (cached) */
int sony_load_allowed(uint32_t type, uint32_t crid);	/* GE2D / I2CDriver only under the Sony condition */
/* RAL hook after publishing a fresh module, before 0x7FFE: 1 replaced (tab and *entry rewritten), 0 not a
 * replaced module, -1 stop (unexpected export count / condition) */
int sony_on_load(uint32_t type, uint32_t crid, uint32_t n, uint32_t *tab, uint32_t *entry);
void sony_stage(uint32_t stage);		/* Sony part of HALSetInitStage(2..4) */
#endif
