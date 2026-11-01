/* pv-vz90-dal g4m: KAL event groups, mailboxes, timers. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KAL_OBJ_H
#define KAL_OBJ_H
#include <stdint.h>
uint32_t kal_eg_create(uint32_t *hP, uint32_t tag, uint32_t initial);	/* [r9-4] 0x87 */
uint32_t kal_eg_delete(uint32_t h);					/* 0x88 */
uint32_t kal_eg_clear(uint32_t h, uint32_t bits);			/* 0x86 */
uint32_t kal_eg_signal(uint32_t h, uint32_t bits);			/* 0x8a */
uint32_t kal_eg_read(uint32_t h, uint32_t *bitsP);			/* 0x89 */
uint32_t kal_eg_wait(uint32_t h, const uint32_t *req, uint32_t *bitsP);	/* 0x8b */
uint32_t kal_mb_create(uint32_t *hP, uint32_t tag, uint32_t depth);	/* 0x8d */
uint32_t kal_mb_delete(uint32_t h);					/* 0x8e */
uint32_t kal_mb_send(uint32_t h, uint32_t msg);				/* 0x8f */
uint32_t kal_mb_wait(uint32_t h, uint32_t *msgP, int32_t timeoutMs);	/* 0x90 */
uint32_t kal_timer_create(uint32_t *hP, uint32_t tag, uint32_t cb, uint32_t userData);	/* 0xa9 */
uint32_t kal_timer_delete(uint32_t h);					/* 0xaa */
uint32_t kal_timer_set(uint32_t h, uint32_t ms, uint32_t callerR9);	/* 0xab */
#endif
