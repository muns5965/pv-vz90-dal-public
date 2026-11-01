/* pv-vz90-dal g3pre: FreeRTOS on PV, separate test image (not combined with the Boot probe).
 * Checks: (1) two tasks of equal priority interleave (time slicing, preemption by the tick),
 *         (2) a binary semaphore hands 5 events from task A to task B,
 *         (3) an auto-reload software timer fires at its period,
 *         (4) a lower-priority busy task only runs while the others block,
 *         (5) r0..r12/lr survive tick preemption and SWI yields (regcheck.S).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "interrupt.h"
#include "../src/pv_hw.h"

volatile uint32_t gTaskSwitches;
static SemaphoreHandle_t gSem;
static volatile uint32_t gGiven, gTaken, gTimerFires, gBusyA, gBusyB, gBusyLow, gInterleave;
static volatile char gLastBusy;
volatile uint32_t gRegIters, gRegYields, gRegFail;
void reg_check_task(void *arg);

static void puts_raw(const char *s) { while (*s) PV_UART_DEBUG = (uint8_t)*s++; }
static void hex_raw(uint32_t v) { int i; puts_raw("0x"); for (i = 28; i >= 0; i -= 4) PV_UART_DEBUG = "0123456789abcdef"[(v >> i) & 15]; }
static void say(const char *a, uint32_t v, const char *b)
{
	taskENTER_CRITICAL();
	puts_raw(a); hex_raw(v); puts_raw(b);
	taskEXIT_CRITICAL();
}

void g3_assert(const char *file, int line) { taskDISABLE_INTERRUPTS(); puts_raw("g3pre: ASSERT "); puts_raw(file); hex_raw((uint32_t)line); puts_raw("\n"); for (;;) ; }
void g3_exception(uint32_t vec, uint32_t lr, uint32_t spsr) { puts_raw("g3pre: EXCEPTION vector "); hex_raw(vec); puts_raw(" lr "); hex_raw(lr); puts_raw(" spsr "); hex_raw(spsr); puts_raw("\n"); for (;;) ; }

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* equal-priority busy tasks: they never block during the first phase, so only the tick can switch them */
static void busy(void *arg)
{
	const char me = (char)(uintptr_t)arg;
	TickType_t until = xTaskGetTickCount() + 30;		/* 300 ms */

	while (xTaskGetTickCount() < until) {
		if (gLastBusy && gLastBusy != me)
			gInterleave++;
		gLastBusy = me;
		if (me == 'A') gBusyA++; else gBusyB++;
	}
	say("g3pre: busy task ", (uint32_t)me, " done\n");
	vTaskSuspend(NULL);
}

static void giver(void *arg)
{
	int i;
	(void)arg;
	vTaskDelay(40);
	for (i = 0; i < 5; i++) {
		vTaskDelay(10);
		gGiven++;
		say("g3pre: give #", gGiven, "\n");
		xSemaphoreGive(gSem);
	}
	vTaskSuspend(NULL);
}

static void taker(void *arg)
{
	(void)arg;
	for (;;) {
		if (xSemaphoreTake(gSem, portMAX_DELAY) == pdTRUE) {
			gTaken++;
			say("g3pre: take #", gTaken, "\n");
		}
	}
}

static void low(void *arg)
{
	(void)arg;
	for (;;)
		gBusyLow++;
}

static void tmr(TimerHandle_t t)
{
	(void)t;
	gTimerFires++;
}

static void check(void *arg)
{
	TickType_t t0;
	uint32_t fires0, ok;
	(void)arg;

	vTaskDelay(100);				/* 1 s: busy phase (0.3 s) and 5 gives (0.4..0.9 s) are over */
	t0 = xTaskGetTickCount();
	fires0 = gTimerFires;
	vTaskDelay(100);				/* timer period 5 ticks -> expect 20 fires in this second */
	taskENTER_CRITICAL();
	puts_raw("g3pre: ticks "); hex_raw(xTaskGetTickCount());
	puts_raw(" switches "); hex_raw(gTaskSwitches);
	puts_raw("\ng3pre: busyA "); hex_raw(gBusyA); puts_raw(" busyB "); hex_raw(gBusyB);
	puts_raw(" interleave "); hex_raw(gInterleave); puts_raw(" low "); hex_raw(gBusyLow);
	puts_raw("\ng3pre: given "); hex_raw(gGiven); puts_raw(" taken "); hex_raw(gTaken);
	puts_raw("\ng3pre: regcheck iterations "); hex_raw(gRegIters); puts_raw(" swi yields "); hex_raw(gRegYields);
	puts_raw(" fail "); hex_raw(gRegFail);
	puts_raw("\ng3pre: timer fires in 100 ticks "); hex_raw(gTimerFires - fires0); puts_raw(" (window "); hex_raw(xTaskGetTickCount() - t0); puts_raw(")\n");
	ok = gInterleave >= 2 && gBusyA && gBusyB && gGiven == 5 && gTaken == 5 &&
	     (gTimerFires - fires0) >= 19 && (gTimerFires - fires0) <= 21 && gBusyLow &&
	     !gRegFail && gRegIters > 0x10000 && gRegYields > 0;
	puts_raw(ok ? "g3pre: PASS\n" : "g3pre: FAIL\n");
	taskEXIT_CRITICAL();
	vTaskSuspend(NULL);
}

int main(void)
{
	TimerHandle_t t;

	puts_raw("g3pre: FreeRTOS " tskKERNEL_VERSION_NUMBER " on PV, tick 10 ms\n");
	pic_init();
	gSem = xSemaphoreCreateBinary();
	t = xTimerCreate("t50", 5, pdTRUE, NULL, tmr);
	configASSERT(gSem && t);
	xTimerStart(t, 0);
	xTaskCreate(busy, "busyA", 256, (void *)'A', 2, NULL);
	xTaskCreate(busy, "busyB", 256, (void *)'B', 2, NULL);
	xTaskCreate(giver, "give", 256, NULL, 2, NULL);
	xTaskCreate(taker, "take", 256, NULL, 2, NULL);
	xTaskCreate(low, "low", 256, NULL, 1, NULL);
	xTaskCreate(reg_check_task, "regs", 256, NULL, 1, NULL);	/* time-sliced with "low" */
	xTaskCreate(check, "check", 256, NULL, 4, NULL);
	vTaskStartScheduler();
	puts_raw("g3pre: scheduler returned (heap?)\n");
	for (;;) ;
}
