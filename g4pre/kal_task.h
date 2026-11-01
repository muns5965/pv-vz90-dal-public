/* pv-vz90-dal g4pre: Palm KAL task-ID layer on top of FreeRTOS. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KAL_TASK_H
#define KAL_TASK_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
/* TEMPORARY ID RULE (stage g4-pre): KAL task IDs are assigned 1, 2, 3, ... in registration order. The numbering the
 * reference uses (Boot's task reads back ID 4) is not yet explained; no ID value is hard-coded. */
#define KAL_ERR_NOT_A_KAL_TASK	0x0000ffffu	/* TEMPORARY error value: the real KAL error code is not yet known */
uint32_t kal_task_register(TaskHandle_t h);
/* register with KAL attributes: user-data word, Palm priority (lower = more urgent), 4CC tag (g4i-fix) */
uint32_t kal_task_register_attrs(TaskHandle_t h, uint32_t userData, uint32_t palmPrio, uint32_t tag);
uint32_t kal_task_get_current_id(uint32_t *tidP);		/* [r9-4] slot 0x9f, KALTaskGetCurrentID(uint32_t *tidP) */
/* [r9-4] slot 0xa0, KALTaskGetInfo(uint32_t tid, info): 36-byte record, see kal_task.c. For a query about the calling
 * task, callerSp/callerResume are the caller's sp and return address at the call (taken from the trap frame). */
#define KAL_ERR_ITEM_NOT_FOUND	0xf00cu
#define KAL_ERR_GENERIC			0xf009u
uint32_t kal_task_get_info(uint32_t tid, uint8_t *info, uint32_t callerSp, uint32_t callerResume);
uint32_t kal_task_switching(uint32_t enable);	/* [r9-4] 0xa4 KALTaskSwitching(Boolean enable) */
/* g4m: task life cycle, [r9-4] 0x9c..0xa7 (local functional specs KALSync §5, KALRest §3) */
uint32_t kal_prio_map(uint32_t palmPrio);
uint32_t kal_task_create(uint32_t *tidP, const uint32_t *rec, uint32_t creatorR9);	/* 0x9c */
uint32_t kal_task_start(uint32_t tid, uint32_t param);		/* 0xa3 */
uint32_t kal_task_delay(uint32_t ms);					/* 0x9d */
uint32_t kal_task_delete(uint32_t tid);				/* 0x9e */
uint32_t kal_task_suspend(uint32_t tid);				/* 0xa2 */
uint32_t kal_task_resume(uint32_t tid);				/* 0xa1 */
uint32_t kal_task_wait(int32_t timeoutMs);				/* 0xa5 */
uint32_t kal_task_wait_clr(void);					/* 0xa6 */
uint32_t kal_task_wake(uint32_t tid);					/* 0xa7 */
int kal_task_is_kal(TaskHandle_t h);
#endif
