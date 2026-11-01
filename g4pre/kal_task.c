/* pv-vz90-dal g4pre: Palm KAL task IDs mapped onto FreeRTOS task handles.
 * Requirement (from the slot specification, docs/INTERFACE.md §9): KALTaskGetCurrentID(tidP) writes the calling
 * task's KAL ID to *tidP and returns 0 (errNone). SPDX-License-Identifier: GPL-3.0-or-later */
#include "kal_task.h"
#include "kal_sync.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);

#define KAL_MAX_TASKS	31
#define KAL_WAKE_MAX	1023u
#define NOTE_TASKWAIT	2		/* notification index used as the KALTaskWait counter */
enum { ST_RUN = 0, ST_DORMANT, ST_SUSPENDED };

static struct {
	TaskHandle_t handle;
	uint32_t id;
	int hasAttrs;			/* created with KAL attributes (user data, Palm priority, tag) */
	uint32_t userData, palmPrio, tag;
	uint32_t entry, r9, param, state;	/* KALTaskCreate'd tasks (g4m) */
	void *stack;
} gKalTasks[KAL_MAX_TASKS];
static uint32_t gKalNextId = 1;

uint32_t kal_task_register_attrs(TaskHandle_t h, uint32_t userData, uint32_t palmPrio, uint32_t tag)
{
	uint32_t i, id = kal_task_register(h);

	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (gKalTasks[i].handle == h) {
			gKalTasks[i].hasAttrs = 1;
			gKalTasks[i].userData = userData;
			gKalTasks[i].palmPrio = palmPrio;
			gKalTasks[i].tag = tag;
		}
	return id;
}

uint32_t kal_task_register(TaskHandle_t h)
{
	uint32_t i, id = 0;

	taskENTER_CRITICAL();
	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (!gKalTasks[i].handle) {
			gKalTasks[i].handle = h;
			gKalTasks[i].id = id = gKalNextId++;
			break;
		}
	taskEXIT_CRITICAL();
	return id;
}

uint32_t kal_task_get_current_id(uint32_t *tidP)
{
	TaskHandle_t me = xTaskGetCurrentTaskHandle();
	uint32_t i;

	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (gKalTasks[i].handle == me) {
			if (tidP)
				*tidP = gKalTasks[i].id;
			return 0;
		}
	return KAL_ERR_NOT_A_KAL_TASK;
}

/* KALTaskGetInfo (stage g4a). Requirements: local functional spec (analyst role) + reference observation
 * (docs/INTERFACE.md §11). Record, 36 bytes, little-endian; only +0x00..+0x17 may be written, only on success:
 *   +0x04 resume address, +0x08 stack pointer, +0x0C = (lowest stack address - value at +0x08) mod 2^32
 *   (Boot reads +0x08 and +0x0C and passes their sum on, i.e. the stack's lowest address).
 *   +0x00 user data, +0x10 priority (Palm scale, encoded), +0x14 tag: written for tasks registered with KAL
 *   attributes (the Boot task since g4i-fix); tasks registered without them leave these fields untouched.
 * Errors: tid 0 / unknown tid -> 0xF00C, nothing written; NULL record -> 0xF009, nothing written (our choice: the
 * reference does not check). */
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint32_t kal_task_get_info(uint32_t tid, uint8_t *info, uint32_t callerSp, uint32_t callerResume)
{
	TaskHandle_t h = NULL;
	TaskStatus_t st;
	uint32_t i, sp, resume, k = KAL_MAX_TASKS;

	if (!tid)
		return KAL_ERR_ITEM_NOT_FOUND;
	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (gKalTasks[i].handle && gKalTasks[i].id == tid) {
			h = gKalTasks[i].handle;
			k = i;
		}
	if (!h)
		return KAL_ERR_ITEM_NOT_FOUND;
	if (!info)
		return KAL_ERR_GENERIC;
	vTaskGetInfo(h, &st, pdFALSE, eInvalid);
	if (h == xTaskGetCurrentTaskHandle()) {
		sp = callerSp;
		resume = callerResume;
	}
	else {
		/* switched-out task: saved context of the FreeRTOS ARM926 port, top of stack = first TCB member:
		 * [0] critical nesting, [1] SPSR, [2..16] r0..r14, [17] pc + 4 (port.c pxPortInitialiseStack) */
		const uint32_t *frame = *(const uint32_t * const *)h;

		sp = frame[15];
		resume = (frame[17] - 4) & ~1u;
	}
	put32(info + 0x04, resume);
	put32(info + 0x08, sp);
	put32(info + 0x0c, (uint32_t)st.pxStackBase - sp);
	if (gKalTasks[k].hasAttrs) {		/* g4i-fix: fields that exist for tasks created with KAL attributes */
		put32(info + 0x00, gKalTasks[k].userData);
		put32(info + 0x10, (0x200u - gKalTasks[k].palmPrio) & 0xffu);	/* encoding as observed in the reference record */
		put32(info + 0x14, gKalTasks[k].tag);
	}
	return 0;
}

/* KALTaskSwitching (stage g4g): false = disable task switching (nested, 8-bit counter), true = re-enable; at nesting
 * level 0 the scheduler resumes; an extra enable returns 0xF009. Mapped onto vTaskSuspendAll()/xTaskResumeAll(),
 * which keep interrupts serviced. */
static uint8_t gSwitchOff;

uint32_t kal_task_switching(uint32_t enable)
{
	if (!(enable & 0xff)) {
		vTaskSuspendAll();
		gSwitchOff++;
		return 0;
	}
	if (!gSwitchOff)
		return KAL_ERR_GENERIC;
	gSwitchOff--;
	xTaskResumeAll();
	return 0;
}

/* ---- g4m: task life cycle (local functional specs KALSync §5, KALRest §3) ---- */

/* Palm priority (lower = more urgent, 0..0x1ff) -> FreeRTOS priority (higher = more urgent), monotonic:
 * 0..255 -> 29..2, 256..0x1ff -> 1; 30 and 31 stay above every OS task (hardware events, KAL timers) */
uint32_t kal_prio_map(uint32_t palm)
{
	return palm > 255 ? 1 : 2 + (255 - palm) * 27 / 255;
}

static int slot_of_id(uint32_t tid)
{
	int i;

	if (!tid)
		return -1;
	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (gKalTasks[i].handle && gKalTasks[i].id == tid)
			return i;
	return -1;
}

static int slot_of_handle(TaskHandle_t h)
{
	int i;

	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (gKalTasks[i].handle == h)
			return i;
	return -1;
}

static void release_slot(int k)
{
	taskENTER_CRITICAL();
	gKalTasks[k].handle = NULL;
	gKalTasks[k].hasAttrs = 0;
	gKalTasks[k].state = ST_RUN;
	taskEXIT_CRITICAL();
}

static void trampoline(void *arg)
{
	int k = (int)(uint32_t)arg;
	uint32_t id = gKalTasks[k].id;

	probe_call_with_r9(gKalTasks[k].entry, gKalTasks[k].r9, gKalTasks[k].param, 0);
	dbg_puts("g4m: KAL task ");
	dbg_hex(id);
	dbg_puts(" entry returned: task ends\n");
	release_slot(k);
	vTaskDelete(NULL);
}

uint32_t kal_task_create(uint32_t *tidP, const uint32_t *rec, uint32_t creatorR9)
{
	uint32_t stack = (rec[2] + 7) & ~7u, id, i;
	TaskHandle_t h;
	int k = -1;

	if (!tidP || !rec)
		return KAL_ERR_NULL;
	if (rec[3] > 0x1ff)
		return KAL_ERR_GENERIC;
	if (stack < 1024)
		stack = 1024;
	taskENTER_CRITICAL();
	for (i = 0; i < KAL_MAX_TASKS; i++)
		if (!gKalTasks[i].handle) {
			k = (int)i;
			gKalTasks[i].handle = (TaskHandle_t)1;	/* reserved until the FreeRTOS task exists */
			break;
		}
	taskEXIT_CRITICAL();
	if (k < 0)
		return KAL_ERR_NO_SPACE;
	gKalTasks[k].userData = rec[0];
	gKalTasks[k].entry = rec[1];
	gKalTasks[k].palmPrio = rec[3];
	gKalTasks[k].tag = rec[4];
	gKalTasks[k].r9 = creatorR9;
	gKalTasks[k].state = ST_DORMANT;
	gKalTasks[k].hasAttrs = 1;
	vTaskSuspendAll();			/* the new task must not run before it is suspended (dormant) */
	if (xTaskCreate(trampoline, "kal", (uint16_t)(stack / 4 > 0xffff ? 0xffff : stack / 4), (void *)(uint32_t)k,
			kal_prio_map(rec[3]), &h) != pdPASS) {
		xTaskResumeAll();
		gKalTasks[k].handle = NULL;
		return KAL_ERR_NO_SPACE;
	}
	vTaskSuspend(h);
	taskENTER_CRITICAL();
	gKalTasks[k].handle = h;
	gKalTasks[k].id = id = gKalNextId++;
	taskEXIT_CRITICAL();
	xTaskResumeAll();
	*tidP = id;
	return 0;
}

uint32_t kal_task_start(uint32_t tid, uint32_t param)
{
	int k = slot_of_id(tid);

	if (k < 0)
		return KAL_ERR_ITEM_NOT_FOUND;
	if (gKalTasks[k].state != ST_DORMANT)
		return KAL_ERR_GENERIC;
	gKalTasks[k].param = param;
	gKalTasks[k].state = ST_RUN;
	vTaskResume(gKalTasks[k].handle);
	return 0;
}

static TickType_t ms_ticks(uint32_t ms)
{
	return (TickType_t)((ms * (uint64_t)configTICK_RATE_HZ + 999) / 1000);
}

uint32_t kal_task_delay(uint32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED)	/* g4p: a delay cannot switch away while switching is off */
		return 0;
	if (ms >= 0x80000000u) {
		for (;;)
			vTaskDelay(portMAX_DELAY);
	}
	vTaskDelay(ms ? ms_ticks(ms) : 1);
	return KAL_ERR_TIMEOUT;			/* g4w: a completed delay returns 0x0501, as measured on the reference
						 * (288 of 288 calls in the theme runs); the delay itself is unchanged */
}

uint32_t kal_task_delete(uint32_t tid)
{
	int k = slot_of_id(tid);
	TaskHandle_t h;

	if (k < 0)
		return KAL_ERR_ITEM_NOT_FOUND;
	h = gKalTasks[k].handle;
	release_slot(k);
	vTaskDelete(h == xTaskGetCurrentTaskHandle() ? NULL : h);
	return 0;
}

uint32_t kal_task_suspend(uint32_t tid)
{
	int k = slot_of_id(tid);

	if (k < 0)
		return KAL_ERR_ITEM_NOT_FOUND;
	if (gKalTasks[k].state != ST_RUN)
		return KAL_ERR_GENERIC;
	gKalTasks[k].state = ST_SUSPENDED;
	vTaskSuspend(gKalTasks[k].handle);
	return 0;
}

uint32_t kal_task_resume(uint32_t tid)
{
	int k = slot_of_id(tid);

	if (k < 0)
		return KAL_ERR_ITEM_NOT_FOUND;
	if (gKalTasks[k].state != ST_SUSPENDED)
		return KAL_ERR_GENERIC;
	gKalTasks[k].state = ST_RUN;
	vTaskResume(gKalTasks[k].handle);
	return 0;
}

uint32_t kal_task_wait(int32_t timeoutMs)
{
	TickType_t t = timeoutMs < 0 ? portMAX_DELAY : ms_ticks((uint32_t)timeoutMs);

	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED)	/* g4p: poll only while switching is off */
		t = 0;
	return ulTaskGenericNotifyTake(NOTE_TASKWAIT, pdFALSE, t) ? 0 : KAL_ERR_TIMEOUT;
}

uint32_t kal_task_wait_clr(void)
{
	ulTaskNotifyValueClearIndexed(NULL, NOTE_TASKWAIT, 0xffffffffu);
	xTaskNotifyStateClearIndexed(NULL, NOTE_TASKWAIT);
	return 0;
}

uint32_t kal_task_wake(uint32_t tid)
{
	int k = slot_of_id(tid);
	TaskHandle_t h;
	uint32_t v;

	if (k < 0)
		return KAL_ERR_ITEM_NOT_FOUND;
	h = gKalTasks[k].handle;
	taskENTER_CRITICAL();
	v = ulTaskNotifyValueClearIndexed(h, NOTE_TASKWAIT, 0);	/* read without changing */
	taskEXIT_CRITICAL();
	if (v < KAL_WAKE_MAX)
		xTaskNotifyGiveIndexed(h, NOTE_TASKWAIT);
	return 0;
}

int kal_task_is_kal(TaskHandle_t h)
{
	return slot_of_handle(h) >= 0;
}
