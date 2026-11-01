/* pv-vz90-dal g4g: KAL mutexes and semaphores on FreeRTOS. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KAL_SYNC_H
#define KAL_SYNC_H
#include <stdint.h>
/* g4w: timeout code 0x0501 (Palm OS sysErrTimeout = sysErrorClass | 1). Measured on the reference for
 * KALEventGroupWait timeouts (theme runs: 4,453 returns of 0x0501). The analyst spec said 0x0504, which is
 * sysErrNoFreeRAM in the public numbering. The other waits (mutex, semaphore, mailbox, task wait) use the same
 * constant; their timeout paths were not observed on the reference. */
#define KAL_ERR_TIMEOUT		0x0501u
#define KAL_ERR_NO_SPACE	0x0102u
#define KAL_ERR_NULL		0xf002u
#define KAL_ERR_GENERIC		0xf009u
#define KAL_ERR_NOT_FOUND	0xf00cu
#define KAL_ERR_NULL_MUTEX	0xffffu
uint32_t kal_mutex_create(uint32_t *handleP, uint32_t tag);		/* [r9-4] 0x92 */
uint32_t kal_mutex_delete(uint32_t h);							/* 0x93 */
uint32_t kal_mutex_release(uint32_t h);							/* 0x94 */
uint32_t kal_mutex_reserve(uint32_t h, int32_t timeoutMs);		/* 0x95 */
uint32_t kal_sem_create(uint32_t *handleP, uint32_t tag, uint32_t initial);	/* 0x97 */
uint32_t kal_sem_delete(uint32_t h);							/* 0x98 */
uint32_t kal_sem_signal(uint32_t h);							/* 0x99 */
uint32_t kal_sem_wait(uint32_t h, int32_t timeoutMs);			/* 0x9a */
#endif
