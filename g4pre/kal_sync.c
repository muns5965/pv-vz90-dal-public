/* pv-vz90-dal g4g: KAL mutexes and counting semaphores mapped onto FreeRTOS objects. Requirements: local functional
 * spec (analyst role), docs/INTERFACE.md §19. KAL handles are opaque non-zero values from one counter shared by all
 * object kinds; a small table maps each handle to its FreeRTOS object.
 * Known difference: FreeRTOS orders waiters by task priority, the reference serves them in arrival order.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "semphr.h"
#include "kal_sync.h"

#define KAL_MAX_MUTEX	32
#define KAL_MAX_SEM		32

struct KalObj {
	uint32_t handle;
	uint32_t tag;
	SemaphoreHandle_t obj;
};

static struct KalObj gMutex[KAL_MAX_MUTEX], gSem[KAL_MAX_SEM];
static uint32_t gNextHandle = 1;

static uint32_t new_handle(void)
{
	uint32_t h = gNextHandle++;

	if (gNextHandle > 0x7fffffffu)
		gNextHandle = 1;
	return h;
}

/* g4m: the same counter for event groups, mailboxes and timers (one counter for all object kinds) */
uint32_t kal_new_handle(void)
{
	return new_handle();
}

/* ms -> FreeRTOS ticks: negative = forever, 0 = no wait, otherwise rounded up */
static TickType_t to_ticks(int32_t ms)
{
	/* g4p: while KALTaskSwitching(off) is in force a wait must not block (the reference keeps the caller running);
	 * FreeRTOS forbids blocking with the scheduler suspended, so the wait becomes a poll */
	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED)
		return 0;
	if (ms < 0)
		return portMAX_DELAY;
	return (TickType_t)(((uint32_t)ms * configTICK_RATE_HZ + 999) / 1000);
}

static struct KalObj *find(struct KalObj *tab, int n, uint32_t h)
{
	int i;

	if (!h)
		return NULL;
	for (i = 0; i < n; i++)
		if (tab[i].handle == h)
			return &tab[i];
	return NULL;
}

static uint32_t create(struct KalObj *tab, int n, uint32_t *handleP, uint32_t tag, SemaphoreHandle_t obj)
{
	int i;

	if (!obj)
		return KAL_ERR_NO_SPACE;
	taskENTER_CRITICAL();
	for (i = 0; i < n; i++)
		if (!tab[i].handle) {
			tab[i].handle = new_handle();
			tab[i].tag = tag;
			tab[i].obj = obj;
			*handleP = tab[i].handle;
			break;
		}
	taskEXIT_CRITICAL();
	if (i == n) {
		vSemaphoreDelete(obj);
		return KAL_ERR_NO_SPACE;
	}
	return 0;
}

static uint32_t destroy(struct KalObj *o)
{
	SemaphoreHandle_t obj = o->obj;

	o->handle = 0;
	o->obj = NULL;
	vSemaphoreDelete(obj);	/* waiters are not released with an error here: not yet needed (no caller seen) */
	return 0;
}

uint32_t kal_mutex_create(uint32_t *handleP, uint32_t tag)
{
	if (!handleP)
		return KAL_ERR_NULL;
	return create(gMutex, KAL_MAX_MUTEX, handleP, tag, xSemaphoreCreateRecursiveMutex());
}

uint32_t kal_mutex_delete(uint32_t h)
{
	struct KalObj *o = find(gMutex, KAL_MAX_MUTEX, h);

	return o ? destroy(o) : KAL_ERR_NOT_FOUND;
}

uint32_t kal_mutex_reserve(uint32_t h, int32_t timeoutMs)
{
	struct KalObj *o;

	if (!h)
		return KAL_ERR_NULL_MUTEX;
	o = find(gMutex, KAL_MAX_MUTEX, h);
	if (!o)
		return KAL_ERR_NOT_FOUND;
	return xSemaphoreTakeRecursive(o->obj, to_ticks(timeoutMs)) == pdTRUE ? 0 : KAL_ERR_TIMEOUT;
}

uint32_t kal_mutex_release(uint32_t h)
{
	struct KalObj *o;

	if (!h)
		return KAL_ERR_NULL_MUTEX;
	o = find(gMutex, KAL_MAX_MUTEX, h);
	if (!o)
		return KAL_ERR_NOT_FOUND;
	return xSemaphoreGiveRecursive(o->obj) == pdTRUE ? 0 : KAL_ERR_GENERIC;	/* not the owner */
}

uint32_t kal_sem_create(uint32_t *handleP, uint32_t tag, uint32_t initial)
{
	if (!handleP)
		return KAL_ERR_NULL;
	return create(gSem, KAL_MAX_SEM, handleP, tag, xSemaphoreCreateCounting(0xffffffffu, initial));
}

uint32_t kal_sem_delete(uint32_t h)
{
	struct KalObj *o = find(gSem, KAL_MAX_SEM, h);

	return o ? destroy(o) : KAL_ERR_NOT_FOUND;
}

uint32_t kal_sem_signal(uint32_t h)
{
	struct KalObj *o = find(gSem, KAL_MAX_SEM, h);

	if (!o)
		return KAL_ERR_NOT_FOUND;
	return xSemaphoreGive(o->obj) == pdTRUE ? 0 : KAL_ERR_GENERIC;
}

uint32_t kal_sem_wait(uint32_t h, int32_t timeoutMs)
{
	struct KalObj *o = find(gSem, KAL_MAX_SEM, h);

	if (!o)
		return KAL_ERR_NOT_FOUND;
	return xSemaphoreTake(o->obj, to_ticks(timeoutMs)) == pdTRUE ? 0 : KAL_ERR_TIMEOUT;
}
