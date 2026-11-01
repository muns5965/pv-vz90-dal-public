/* pv-vz90-dal g4j: HALSetInitStage and the driver-event path. Requirements: local functional specs (analyst role,
 * HALMisc / HALInput); PV device facts in src/pv_hw.h. See docs/INTERFACE.md §24.
 * Stage 0: recorded. Stage 1: driver-event queue (128 entries) + hardware-event task, RTC interrupt acknowledged and
 * left masked, input driver up (key word latched, key IRQ cleared, touch and key IRQs unmasked). Stages 2-4: recorded;
 * the Sony library hook (stage 2) and the feature settings (stage 3) are NOT implemented yet (logged).
 * Callbacks registered with HALEventRegisterCallBack are called from the event task with the registering module's r9
 * and task switching disabled. Event payload layouts are taken from the spec and are not yet exercised by a test.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "interrupt.h"
#include "hal_events.h"
#include "hal_time.h"
#include "ral.h"
#include "sony.h"
#include "kal_task.h"
#include "hal_input.h"
#include "../src/pv_hw.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);

#define EV_PEN	0
#define EV_KEY	1
#define EV_CLOCK	2

struct DrvEvent {
	uint8_t kind;		/* EV_PEN: x, y; EV_KEY: bits changed (a), new state (b) */
	int16_t x, y;
	uint32_t a, b;
};

static uint32_t gStage = 0xffffffffu, gCb[3], gCbR9[3];
static QueueHandle_t gDrvQ;
static uint32_t gKeysLatched, gKeysTracked, gTouchLast = 0xffffffffu;
static int gInputUp;
static volatile uint32_t gDrvDropped;

static uint32_t map_keys(uint32_t pv)
{
	return (pv & ~0x08000000u) | ((pv & 0x08000000u) ? 0x04000000u : 0);
}

static void keys_isr(void)
{
	struct DrvEvent e;
	uint32_t now;

	PV_KEYS_STATE = 0;			/* clear the interrupt first, then read, so no change is lost */
	now = map_keys(PV_KEYS_STATE);
	if (now == gKeysLatched)
		return;
	e.kind = EV_KEY;
	e.a = now ^ gKeysLatched;
	e.b = now;
	gKeysLatched = now;
	if (xQueueSendFromISR(gDrvQ, &e, NULL) != pdTRUE)
		gDrvDropped++;
}

static void touch_isr(void)
{
	struct DrvEvent e;
	uint32_t v = PV_TOUCH_STATE;		/* one read: returns the sample and clears the interrupt */

	if (v == gTouchLast)
		return;
	gTouchLast = v;
	e.kind = EV_PEN;
	e.x = (int16_t)(v & 0xffff);
	e.y = (int16_t)(v >> 16);
	if (xQueueSendFromISR(gDrvQ, &e, NULL) != pdTRUE)
		gDrvDropped++;
}

static volatile uint32_t gAlarm, gAlarmSet;

static void rtc_isr(void)
{
	struct DrvEvent e;

	PV_RTC_CLEARIRQ = 0;
	e.kind = EV_CLOCK;
	if (xQueueSendFromISR(gDrvQ, &e, NULL) != pdTRUE)
		gDrvDropped++;
}

static void call_cb(uint32_t type, uint32_t a0, uint32_t a1)
{
	if (!gCb[type])
		return;
	vTaskSuspendAll();
	probe_call_with_r9(gCb[type], gCbR9[type], a0, a1);
	xTaskResumeAll();
}

/* g4r: key events to the OS (local spec HALInput "Key event generation"): a press of a key that is set in the
 * HALKeySetMask mask posts {chr, keyCode 0, modifiers} to the key callback (type 1); releases post nothing; held
 * masked keys other than power repeat every 100 ms with autoRepeatKeyMask. Character codes and modifier bits are
 * the public Palm OS virtual-character values. */
#define COMMAND_KEY_MASK	0x0008u
#define AUTO_REPEAT_MASK	0x0040u
#define KEY_REPEAT_MS		100u

static const struct { uint32_t bit; uint16_t chr, mods; } gKeyMap[] = {
	{ 0x00000001u, 0x0208, COMMAND_KEY_MASK },	/* power: vchrHardPower */
	{ 0x00000002u, 0x000b, 0 },			/* page up: vchrPageUp */
	{ 0x00000004u, 0x000c, 0 },			/* page down: vchrPageDown */
	{ 0x00000008u, 0x0204, COMMAND_KEY_MASK },	/* hard1..hard3 */
	{ 0x00000010u, 0x0205, COMMAND_KEY_MASK },
	{ 0x00000020u, 0x0206, COMMAND_KEY_MASK },
	{ 0x00000040u, 0x0207, 0 },			/* hard4 (no commandKeyMask, as in the reference) */
	{ 0x00000080u, 0x0209, COMMAND_KEY_MASK },	/* cradle */
	/* Sony jog and rocker bits never appear on PV: no entries */
};

static void post_key(uint16_t chr, uint16_t mods)
{
	uint16_t ev[3] = { chr, 0, mods };

	call_cb(EV_KEY, (uint32_t)ev, 0);
}

static void key_change(uint32_t changed, uint32_t now)
{
	uint32_t mask = hal_key_get_mask(), i;

	for (i = 0; i < sizeof(gKeyMap) / sizeof(gKeyMap[0]); i++)
		if ((changed & gKeyMap[i].bit & mask) && (now & gKeyMap[i].bit))
			post_key(gKeyMap[i].chr, gKeyMap[i].mods);
}

static void key_repeat(uint32_t held)
{
	uint32_t mask = hal_key_get_mask(), i;

	for (i = 0; i < sizeof(gKeyMap) / sizeof(gKeyMap[0]); i++)
		if ((held & gKeyMap[i].bit & mask) && gKeyMap[i].bit != 0x1u)
			post_key(gKeyMap[i].chr, gKeyMap[i].mods | AUTO_REPEAT_MASK);
}

static void event_task(void *arg)
{
	struct DrvEvent e;
	(void)arg;

	for (;;) {
		TickType_t wait = (gKeysTracked & hal_key_get_mask() & ~0x1u) ? pdMS_TO_TICKS(KEY_REPEAT_MS) : portMAX_DELAY;

		if (xQueueReceive(gDrvQ, &e, wait) != pdTRUE) {
			key_repeat(gKeysTracked);
			continue;
		}
		if (e.kind == EV_PEN) {
			int16_t pt[2] = { e.x, e.y };	/* raw digitizer units; pen up = -1, -1 */
			static uint32_t shown;

			if (shown++ < 2) {
				dbg_puts("g4r: pen event ");
				dbg_hex(((uint32_t)(uint16_t)e.x << 16) | (uint16_t)e.y);
				dbg_puts(gCb[EV_PEN] ? " -> callback\n" : " (no callback registered)\n");
			}
			call_cb(EV_PEN, (uint32_t)pt, 0);
			if (shown <= 2)
				dbg_puts("g4r: pen callback returned\n");
		} else if (e.kind == EV_KEY) {
			gKeysTracked = e.b;
			key_change(e.a, e.b);
		} else if (e.kind == EV_CLOCK && gAlarm && hal_time_get_seconds() >= gAlarm) {
			gAlarm = 0;			/* one-shot; HALTimeGetAlarm keeps reporting the set value */
			pic_disableInterrupt(PV_IRQ_RTC);
			dbg_puts("g4o: alarm due -> clock event\n");
			call_cb(EV_CLOCK, 0, 0);
		}
	}
}

static int bring_up_drivers(void)
{
	gDrvQ = xQueueCreate(128, sizeof(struct DrvEvent));
	if (!gDrvQ || xTaskCreate(event_task, "hwev", 512, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
		return -1;
	PV_RTC_CLEARIRQ = 0;			/* acknowledge a pending RTC interrupt; source 2 stays masked */
	pic_disableInterrupt(PV_IRQ_RTC);
	gKeysLatched = map_keys(PV_KEYS_STATE);
	gKeysTracked = gKeysLatched;		/* seeded from the baseline (spec recommendation) */
	PV_KEYS_STATE = 0;			/* clear the key interrupt */
	pic_registerIrq(PV_IRQ_RTC, rtc_isr, 0);
	pic_registerIrq(PV_IRQ_KEYS, keys_isr, 0);
	pic_registerIrq(PV_IRQ_TOUCH, touch_isr, 0);
	pic_enableInterrupt(PV_IRQ_TOUCH);
	pic_enableInterrupt(PV_IRQ_KEYS);
	gInputUp = 1;
	return 0;
}

uint32_t hal_set_init_stage(uint32_t stage)
{
	stage &= 0xff;
	gStage = stage;
	dbg_puts("g4j: HALSetInitStage(");
	dbg_hex(stage);
	dbg_puts(")");
	switch (stage) {
	case 1:
		dbg_puts(bring_up_drivers() ? " driver bring-up FAILED\n" : " drivers up: event task, RTC masked, touch+keys unmasked\n");
		ral_memory_stage();
		break;
	case 2: case 3: case 4:
		dbg_puts(" Sony hook and generic features\n");
		sony_stage(stage);
		break;
	default:
		dbg_puts(" recorded\n");
		break;
	}
	return 0;
}

uint32_t hal_get_init_stage(void)
{
	return gStage;
}

uint32_t hal_event_register_callback(uint32_t type, uint32_t cb, uint32_t *oldP, uint32_t r9)
{
	if (type > 2)
		return 0xff00u;
	if (oldP)
		*oldP = gCb[type];
	gCb[type] = cb;
	gCbR9[type] = r9;
	return 0;
}

uint32_t hal_event_post(uint32_t type, const void *data)
{
	if (type != EV_KEY || !data)
		return 0xffffu;
	call_cb(EV_KEY, (uint32_t)data, 0);
	return 0;
}

int hal_input_driver_up(void)
{
	return gInputUp;
}

uint32_t hal_input_tracked_keys(void)
{
	return gKeysTracked;
}

/* g4o: HALTimeGetAlarm / HALTimeSetAlarm (local spec HALRandomTime); resolution 1 s, 1 Hz RTC interrupt only while
 * an alarm is armed; due alarms raise event type 2 (clock) */
uint32_t hal_time_get_alarm(uint32_t *secondsP)
{
	if (secondsP)
		*secondsP = gAlarmSet;
	return 0;
}

uint32_t hal_time_set_alarm(uint32_t seconds)
{
	gAlarmSet = seconds;
	if (!seconds) {
		gAlarm = 0;
		if (gDrvQ)
			pic_disableInterrupt(PV_IRQ_RTC);
		return 0;
	}
	if (seconds <= hal_time_get_seconds()) {
		gAlarm = 0;
		call_cb(EV_CLOCK, 0, 0);		/* already due: raised in the caller's context */
		return 0;
	}
	gAlarm = seconds;
	if (gDrvQ) {
		PV_RTC_CLEARIRQ = 0;
		pic_enableInterrupt(PV_IRQ_RTC);
	}
	return 0;
}
