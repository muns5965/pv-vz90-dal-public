/* pv-vz90-dal g4m: KAL event groups, mailboxes and timers. Requirements: local functional specs (analyst role)
 * KALSync §0/§3/§4 and KALRest §1/§2/§4; see docs/INTERFACE.md §27.
 * Event groups and mailboxes keep their own FIFO waiter lists (FreeRTOS event groups reserve 8 bits, FreeRTOS queues
 * cannot have depth 0 and order waiters by priority). A waiter blocks on its task notification index 1.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "kal_obj.h"
#include "kal_sync.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);
uint32_t kal_new_handle(void);

#define NOTE_OBJ	1
#define MAX_EG		32
#define MAX_MB		32
#define MAX_TMR		64
#define KAL_ERR_MB_FULL	0xf00au
#define TIMER_TASK_PRIO	(configMAX_PRIORITIES - 1)

struct waiter {
	struct waiter *next;
	TaskHandle_t task;
	uint32_t wanted, type;		/* event groups */
	uint32_t value;			/* event state / mailbox word handed over */
	uint32_t result;		/* 0 = satisfied, KAL_ERR_GENERIC = object deleted */
	int done;
};

static TickType_t ticks(int32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED)	/* g4p: no blocking while switching is off */
		return 0;
	if (ms < 0)
		return portMAX_DELAY;
	return (TickType_t)(((uint64_t)(uint32_t)ms * configTICK_RATE_HZ + 999) / 1000);
}

static void enqueue(struct waiter **head, struct waiter *w)
{
	while (*head)
		head = &(*head)->next;
	w->next = NULL;
	*head = w;
}

static void unlink_waiter(struct waiter **head, struct waiter *w)
{
	for (; *head; head = &(*head)->next)
		if (*head == w) {
			*head = w->next;
			return;
		}
}

static void release(struct waiter *w, uint32_t result, uint32_t value)
{
	w->result = result;
	w->value = value;
	w->done = 1;
	xTaskNotifyGiveIndexed(w->task, NOTE_OBJ);
}

/* called inside a critical section (left on return); queues w; returns 0 when released, KAL_ERR_TIMEOUT otherwise */
static uint32_t block(struct waiter **head, struct waiter *w, int32_t timeoutMs)
{
	uint32_t r;

	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) {	/* g4p: cannot block while switching is off */
		taskEXIT_CRITICAL();
		return KAL_ERR_TIMEOUT;
	}
	w->task = xTaskGetCurrentTaskHandle();
	w->done = 0;
	xTaskNotifyStateClearIndexed(NULL, NOTE_OBJ);
	ulTaskNotifyValueClearIndexed(NULL, NOTE_OBJ, 0xffffffffu);
	enqueue(head, w);
	taskEXIT_CRITICAL();
	ulTaskGenericNotifyTake(NOTE_OBJ, pdTRUE, ticks(timeoutMs));
	taskENTER_CRITICAL();
	if (!w->done) {
		unlink_waiter(head, w);
		r = KAL_ERR_TIMEOUT;
	} else
		r = w->result;
	taskEXIT_CRITICAL();
	ulTaskNotifyValueClearIndexed(NULL, NOTE_OBJ, 0xffffffffu);	/* a late notification must not leak into the next wait */
	return r;
}

/* ---- event groups ---- */
static struct eg { uint32_t handle, tag, bits; struct waiter *waiters; } gEg[MAX_EG];

static struct eg *eg_find(uint32_t h)
{
	int i;

	for (i = 0; h && i < MAX_EG; i++)
		if (gEg[i].handle == h)
			return &gEg[i];
	return NULL;
}

static int eg_ok(uint32_t bits, const struct waiter *w)
{
	return w->type == 1 ? (bits & w->wanted) == w->wanted : (bits & w->wanted) != 0;
}

uint32_t kal_eg_create(uint32_t *hP, uint32_t tag, uint32_t initial)
{
	int i;

	if (!hP)
		return KAL_ERR_NULL;
	taskENTER_CRITICAL();
	for (i = 0; i < MAX_EG; i++)
		if (!gEg[i].handle) {
			gEg[i].handle = kal_new_handle();
			gEg[i].tag = tag;
			gEg[i].bits = initial;
			gEg[i].waiters = NULL;
			*hP = gEg[i].handle;
			taskEXIT_CRITICAL();
			return 0;
		}
	taskEXIT_CRITICAL();
	return KAL_ERR_NO_SPACE;
}

uint32_t kal_eg_delete(uint32_t h)
{
	struct eg *e;
	struct waiter *w;

	taskENTER_CRITICAL();
	if (!(e = eg_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	for (w = e->waiters; w; w = w->next)
		release(w, KAL_ERR_GENERIC, 0);
	e->waiters = NULL;
	e->handle = 0;
	taskEXIT_CRITICAL();
	return 0;
}

uint32_t kal_eg_clear(uint32_t h, uint32_t bits)
{
	struct eg *e;

	taskENTER_CRITICAL();
	if (!(e = eg_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	e->bits &= ~bits;
	taskEXIT_CRITICAL();
	return 0;
}

uint32_t kal_eg_signal(uint32_t h, uint32_t bits)
{
	struct eg *e;
	struct waiter **pp;

	taskENTER_CRITICAL();
	if (!(e = eg_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	if (bits & ~e->bits) {				/* only a 0 -> 1 change re-evaluates the waiters */
		e->bits |= bits;
		for (pp = &e->waiters; *pp;) {
			struct waiter *w = *pp;

			if (eg_ok(e->bits, w)) {
				*pp = w->next;
				release(w, 0, e->bits);
			} else
				pp = &w->next;
		}
	}
	taskEXIT_CRITICAL();
	return 0;
}

uint32_t kal_eg_read(uint32_t h, uint32_t *bitsP)
{
	struct eg *e = eg_find(h);

	if (!e)
		return KAL_ERR_NOT_FOUND;
	if (!bitsP)
		return KAL_ERR_NULL;
	*bitsP = e->bits;
	return 0;
}

uint32_t kal_eg_wait(uint32_t h, const uint32_t *req, uint32_t *bitsP)
{
	struct eg *e;
	struct waiter w;
	uint32_t r;

	taskENTER_CRITICAL();
	if (!(e = eg_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	if (!bitsP || !req) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NULL;
	}
	w.wanted = req[0];
	w.type = req[1];
	if (w.type != 1 && w.type != 2) {
		taskEXIT_CRITICAL();
		return KAL_ERR_GENERIC;
	}
	if (!w.wanted || eg_ok(e->bits, &w)) {
		*bitsP = e->bits;
		taskEXIT_CRITICAL();
		return 0;
	}
	if (!req[2]) {
		taskEXIT_CRITICAL();
		return KAL_ERR_TIMEOUT;
	}
	r = block(&e->waiters, &w, (int32_t)req[2]);
	if (!r)
		*bitsP = w.value;
	return r;
}

/* ---- mailboxes ---- */
static struct mb { uint32_t handle, tag, depth, head, count; uint32_t *buf; struct waiter *waiters; } gMb[MAX_MB];

static struct mb *mb_find(uint32_t h)
{
	int i;

	for (i = 0; h && i < MAX_MB; i++)
		if (gMb[i].handle == h)
			return &gMb[i];
	return NULL;
}

uint32_t kal_mb_create(uint32_t *hP, uint32_t tag, uint32_t depth)
{
	uint32_t *buf = NULL;
	int i;

	if (!hP)
		return KAL_ERR_NULL;
	if (depth > 0x00100000u)
		return KAL_ERR_NO_SPACE;
	if (depth && !(buf = pvPortMalloc(depth * 4)))
		return KAL_ERR_NO_SPACE;
	taskENTER_CRITICAL();
	for (i = 0; i < MAX_MB; i++)
		if (!gMb[i].handle) {
			gMb[i].handle = kal_new_handle();
			gMb[i].tag = tag;
			gMb[i].depth = depth;
			gMb[i].head = gMb[i].count = 0;
			gMb[i].buf = buf;
			gMb[i].waiters = NULL;
			*hP = gMb[i].handle;
			taskEXIT_CRITICAL();
			return 0;
		}
	taskEXIT_CRITICAL();
	if (buf)
		vPortFree(buf);
	return KAL_ERR_NO_SPACE;
}

uint32_t kal_mb_delete(uint32_t h)
{
	struct mb *m;
	struct waiter *w;
	uint32_t *buf;

	taskENTER_CRITICAL();
	if (!(m = mb_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	for (w = m->waiters; w; w = w->next)
		release(w, KAL_ERR_GENERIC, 0);
	buf = m->buf;
	m->handle = 0;
	m->waiters = NULL;
	m->buf = NULL;
	taskEXIT_CRITICAL();
	if (buf)
		vPortFree(buf);
	return 0;
}

uint32_t kal_mb_send(uint32_t h, uint32_t msg)
{
	struct mb *m;
	struct waiter *w;

	taskENTER_CRITICAL();
	if (!(m = mb_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	if ((w = m->waiters) != NULL) {		/* oldest waiter gets the word directly */
		m->waiters = w->next;
		release(w, 0, msg);
	} else if (m->count < m->depth) {
		m->buf[(m->head + m->count) % m->depth] = msg;
		m->count++;
	} else {
		taskEXIT_CRITICAL();
		return KAL_ERR_MB_FULL;
	}
	taskEXIT_CRITICAL();
	return 0;
}

uint32_t kal_mb_wait(uint32_t h, uint32_t *msgP, int32_t timeoutMs)
{
	struct mb *m;
	struct waiter w;
	uint32_t r;

	taskENTER_CRITICAL();
	if (!(m = mb_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	if (!msgP) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NULL;
	}
	if (m->count) {
		*msgP = m->buf[m->head];
		m->head = (m->head + 1) % m->depth;
		m->count--;
		taskEXIT_CRITICAL();
		return 0;
	}
	if (!timeoutMs) {
		taskEXIT_CRITICAL();
		return KAL_ERR_TIMEOUT;
	}
	r = block(&m->waiters, &w, timeoutMs);
	if (!r)
		*msgP = w.value;
	return r;
}

/* ---- timers: one-shot, callbacks in one dedicated task above every OS task ---- */
static struct tmr { uint32_t handle, tag, cb, userData, r9, armed; TickType_t due; } gTmr[MAX_TMR];
static TaskHandle_t gTmrTask;

static struct tmr *tmr_find(uint32_t h)
{
	int i;

	for (i = 0; h && i < MAX_TMR; i++)
		if (gTmr[i].handle == h)
			return &gTmr[i];
	return NULL;
}

static void timer_task(void *arg)
{
	(void)arg;
	for (;;) {
		TickType_t now = xTaskGetTickCount(), wait = portMAX_DELAY;
		int i, fired = 0;

		for (i = 0; i < MAX_TMR; i++) {
			uint32_t cb, ud, r9;

			taskENTER_CRITICAL();
			if (!gTmr[i].handle || !gTmr[i].armed) {
				taskEXIT_CRITICAL();
				continue;
			}
			if ((int32_t)(gTmr[i].due - now) > 0) {
				if ((TickType_t)(gTmr[i].due - now) < wait)
					wait = gTmr[i].due - now;
				taskEXIT_CRITICAL();
				continue;
			}
			gTmr[i].armed = 0;
			cb = gTmr[i].cb;
			ud = gTmr[i].userData;
			r9 = gTmr[i].r9;
			taskEXIT_CRITICAL();
			probe_call_with_r9(cb, r9, ud, 0);
			fired = 1;
		}
		if (!fired)
			ulTaskNotifyTake(pdTRUE, wait);
	}
}

uint32_t kal_timer_create(uint32_t *hP, uint32_t tag, uint32_t cb, uint32_t userData)
{
	int i;

	if (!hP)
		return KAL_ERR_NULL;
	if (!gTmrTask && xTaskCreate(timer_task, "kaltmr", 1024, NULL, TIMER_TASK_PRIO, &gTmrTask) != pdPASS) {
		gTmrTask = NULL;
		return KAL_ERR_NO_SPACE;
	}
	taskENTER_CRITICAL();
	for (i = 0; i < MAX_TMR; i++)
		if (!gTmr[i].handle) {
			gTmr[i].handle = kal_new_handle();
			gTmr[i].tag = tag;
			gTmr[i].cb = cb;
			gTmr[i].userData = userData;
			gTmr[i].armed = 0;
			*hP = gTmr[i].handle;
			taskEXIT_CRITICAL();
			return 0;
		}
	taskEXIT_CRITICAL();
	return KAL_ERR_NO_SPACE;
}

uint32_t kal_timer_delete(uint32_t h)
{
	struct tmr *t;

	taskENTER_CRITICAL();
	if (!(t = tmr_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	t->armed = 0;
	t->handle = 0;
	taskEXIT_CRITICAL();
	return 0;
}

uint32_t kal_timer_set(uint32_t h, uint32_t ms, uint32_t callerR9)
{
	struct tmr *t;
	uint64_t tk = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000;

	if (tk > 0x7fffffffu)
		tk = 0x7fffffffu;		/* very long delays must not wrap to "immediate" */
	taskENTER_CRITICAL();
	if (!(t = tmr_find(h))) {
		taskEXIT_CRITICAL();
		return KAL_ERR_NOT_FOUND;
	}
	t->r9 = callerR9;
	t->due = xTaskGetTickCount() + (TickType_t)tk;
	t->armed = 1;
	taskEXIT_CRITICAL();
	xTaskNotifyGive(gTmrTask);
	return 0;
}
