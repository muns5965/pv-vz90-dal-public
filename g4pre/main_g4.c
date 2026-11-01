/* pv-vz90-dal g4pre: first integration of FreeRTOS and the Boot probe. 0x7ffe runs before the scheduler (as in g2e,
 * System stack); Boot export 0 then runs inside a FreeRTOS task that is registered as a KAL task.
 * Implemented slots: [r9-4] 0x9f KALTaskGetCurrentID, 0xa0 KALTaskGetInfo (g4a), 0x51 HALMemoryGetMemoryMap (g4b). Replay rule kept: [r9-4] 0x6d with r0 = 0.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "interrupt.h"
#include "kal_task.h"
#include "hal_memory.h"
#include "hal_display.h"
#include "hal_input.h"
#include "hal_time.h"
#include "kal_sync.h"
#include "hal_events.h"
#include "hal_misc.h"
#include "ral.h"
#include "kal_obj.h"
#include "hal_flp.h"
#include "hal_draw.h"
#include "hal_bootscreen.h"
#include "selftest_draw.h"
#include "../src/pv_hw.h"

int probe_prepare(void);
uint32_t probe_cmd_7ffe(void);
uint32_t probe_export0_addr(void);
uint32_t probe_r9(void);
uint32_t probe_call_in_task(uint32_t entry, uint32_t r9, uint32_t flags);

volatile uint32_t gTaskSwitches;
int gProbeQuiet = 1;			/* g4o: first (slot, caller) pair only, plus summaries */
static void check_boot_block(void);

/* g4o: debug output is collected per task and written one whole line at a time, so lines from tasks that are inside
 * the DAL at the same time do not interleave (before the scheduler runs and in interrupt mode: written directly) */
#define LINE_TASKS	32
#define LINE_MAX	320
#ifndef DAL_LOG
#define DAL_LOG 1	/* debug log on the PV debug UART; build with DAL_LOG=0 for a quiet (faster) image (docs §34) */
#endif

static struct { TaskHandle_t h; uint16_t n; uint8_t mute, drop; char b[LINE_MAX]; } gLine[LINE_TASKS];

/* lines kept even while a repeated (slot, caller) call is being handled quietly */
static int important(const char *b, uint32_t n)
{
	static const char *const keep[] = { "g4k:   RALLoad", "g4k:   RALUnload", "g4k:   module", "g4k: ", "g4o", "g4n",
		"g4j: HALErr", "g4j: HALSet", "g4m:   KALTaskCreate", "g4m:   KALTaskDelete", "g4m:   KALTask", "g2e", "g4l", "g4r", "g4s", "g4u" };
	uint32_t i, j;

	for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
		for (j = 0; keep[i][j] && j < n && b[j] == keep[i][j]; j++)
			;
		if (!keep[i][j])
			return 1;
	}
	return 0;
}

static int line_slot(void);

void probe_mute(int on)
{
	int k;

	if (!DAL_LOG)
		return;
	k = line_slot();

	if (k >= 0)
		gLine[k].mute = (uint8_t)on;
}

static void line_flush(int k)
{
	uint32_t i;

	if (gLine[k].mute && !important(gLine[k].b, gLine[k].n)) {
		gLine[k].n = 0;
		return;
	}
	taskENTER_CRITICAL();
	for (i = 0; i < gLine[k].n; i++)
		PV_UART_DEBUG = (uint8_t)gLine[k].b[i];
	taskEXIT_CRITICAL();
	gLine[k].n = 0;
}

/* this task's line-buffer slot, -1 when output is unbuffered (no scheduler yet, interrupt mode, table full) */
static int line_slot(void)
{
	uint32_t cpsr;
	TaskHandle_t me;
	int k, freeK = -1;

	__asm volatile ("mrs %0, cpsr" : "=r"(cpsr));
	if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED || (cpsr & 0x1f) != 0x1f || (cpsr & 0x80))
		return -1;
	me = xTaskGetCurrentTaskHandle();
	{
		uint32_t cached = (uint32_t)pvTaskGetThreadLocalStoragePointer(me, 0);

		if (cached && gLine[cached - 1].h == me)
			return (int)cached - 1;
	}
	taskENTER_CRITICAL();
	for (k = 0; k < LINE_TASKS; k++) {
		if (gLine[k].h == me)
			break;
		if (freeK < 0 && (!gLine[k].h || !gLine[k].n))
			freeK = k;
	}
	if (k == LINE_TASKS && freeK >= 0) {
		k = freeK;
		gLine[k].h = me;
		gLine[k].n = 0;
		gLine[k].mute = 0;
		gLine[k].drop = 0;
	}
	taskEXIT_CRITICAL();
	if (k < LINE_TASKS)
		vTaskSetThreadLocalStoragePointer(me, 0, (void *)(uint32_t)(k + 1));
	return k == LINE_TASKS ? -1 : k;
}

void dbg_puts(const char *s)
{
	int k;

	if (!DAL_LOG)
		return;
	k = line_slot();

	if (k < 0) {
		while (*s)
			PV_UART_DEBUG = (uint8_t)*s++;
		return;
	}
	if (gLine[k].n == 0 && !gLine[k].drop && gLine[k].mute) {	/* g4r: decide on the first chunk of a muted line */
		uint32_t len = 0;

		while (s[len])
			len++;
		if (!important(s, len))
			gLine[k].drop = 1;
	}
	if (gLine[k].drop) {				/* skip to the end of the line without buffering */
		while (*s)
			if (*s++ == '\n')
				gLine[k].drop = 0;
		return;
	}
	while (*s) {
		gLine[k].b[gLine[k].n++] = *s;
		if (*s++ == '\n' || gLine[k].n == LINE_MAX)
			line_flush(k);
	}
}
void dbg_hex(uint32_t v) { char t[11]; int i; t[0] = '0'; t[1] = 'x'; for (i = 0; i < 8; i++) t[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; t[10] = 0; dbg_puts(t); }
void g3_assert(const char *file, int line) { taskDISABLE_INTERRUPTS(); dbg_puts("g4pre: ASSERT "); dbg_puts(file); dbg_hex((uint32_t)line); dbg_puts("\n"); for (;;) ; }
void g3_exception(uint32_t vec, uint32_t lr, uint32_t spsr) { dbg_puts("g4pre: EXCEPTION vector "); dbg_hex(vec); dbg_puts(" lr "); dbg_hex(lr); dbg_puts(" spsr "); dbg_hex(spsr); dbg_puts("\n"); for (;;) ; }
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define SLOT_ID(tab, slot)	(((uint32_t)(tab) << 12) | (slot))

/* g4m: event groups, mailboxes, task life cycle, timers, GetInfo placeholders */
static int kal_dispatch(uint32_t s, uint32_t *a, uint32_t r9)
{
	static const char *const names[] = { "EventGroupClear", "EventGroupCreate", "EventGroupDelete", "EventGroupRead",
		"EventGroupSignal", "EventGroupWait", "EventGroupGetInfo", "MailboxCreate", "MailboxDelete", "MailboxSend",
		"MailboxWait", "MailboxGetInfo", 0, 0, 0, 0, "MutexGetInfo", 0, 0, 0, 0, "SemaphoreGetInfo", "TaskCreate",
		"TaskDelay", "TaskDelete", 0, 0, "TaskResume", "TaskSuspend", "TaskStart", 0, "TaskWait", "TaskWaitClr",
		"TaskWake", "TaskExit", "TimerCreate", "TimerDelete", "TimerSet", "TimerGetInfo" };
	uint32_t r, quiet = 0;

	if (s < 0x86 || s > 0xac || !names[s - 0x86])
		return 0;
	switch (s) {
	case 0x86: r = kal_eg_clear(a[1], a[2]); quiet = 1; break;
	case 0x87: r = kal_eg_create((uint32_t *)a[1], a[2], a[3]); break;
	case 0x88: r = kal_eg_delete(a[1]); break;
	case 0x89: r = kal_eg_read(a[1], (uint32_t *)a[2]); quiet = 1; break;
	case 0x8a: r = kal_eg_signal(a[1], a[2]); quiet = 1; break;
	case 0x8b: r = kal_eg_wait(a[1], (const uint32_t *)a[2], (uint32_t *)a[3]); quiet = 1; break;
	case 0x8d: r = kal_mb_create((uint32_t *)a[1], a[2], a[3]); break;
	case 0x8e: r = kal_mb_delete(a[1]); break;
	case 0x8f: r = kal_mb_send(a[1], a[2]); quiet = 1; break;
	case 0x90: r = kal_mb_wait(a[1], (uint32_t *)a[2], (int32_t)a[3]); quiet = 1; break;
	case 0x9c: r = kal_task_create((uint32_t *)a[1], (const uint32_t *)a[2], r9); break;
	case 0x9d: r = kal_task_delay(a[1]); quiet = 1; break;
	case 0x9e: r = kal_task_delete(a[1]); break;
	case 0xa1: r = kal_task_resume(a[1]); break;
	case 0xa2: r = kal_task_suspend(a[1]); break;
	case 0xa3: r = kal_task_start(a[1], a[2]); break;
	case 0xa5: r = kal_task_wait((int32_t)a[1]); quiet = 1; break;
	case 0xa6: r = kal_task_wait_clr(); quiet = 1; break;
	case 0xa7: r = kal_task_wake(a[1]); quiet = 1; break;
	case 0xa9: r = kal_timer_create((uint32_t *)a[1], a[2], a[3], a[4]); break;
	case 0xaa: r = kal_timer_delete(a[1]); break;
	case 0xab: r = kal_timer_set(a[1], a[2], r9); quiet = 1; break;
	default:					/* GetInfo placeholders, KALTaskExit: not expected before the Launcher */
		dbg_puts("g4m:   KAL");
		dbg_puts(names[s - 0x86]);
		dbg_puts(" called (placeholder in this DAL) from ");
		dbg_hex(a[5]);
		dbg_puts(" -> 0xf009\n");
		a[1] = KAL_ERR_GENERIC;
		return 1;
	}
	if (!quiet || r) {
		dbg_puts("g4m:   KAL");
		dbg_puts(names[s - 0x86]);
		dbg_puts(" ");
		dbg_hex(a[1]);
		if (s == 0x9c && !r) {
			const uint32_t *rec = (const uint32_t *)a[2];

			dbg_puts(" -> tid ");
			dbg_hex(*(uint32_t *)a[1]);
			dbg_puts(" entry ");
			dbg_hex(rec[1]);
			dbg_puts(" stack ");
			dbg_hex(rec[2]);
			dbg_puts(" palm prio ");
			dbg_hex(rec[3]);
			dbg_puts(" tag ");
			dbg_hex(rec[4]);
		} else if ((s == 0x87 || s == 0x8d || s == 0xa9) && !r) {
			dbg_puts(" tag ");
			dbg_hex(a[2]);
			dbg_puts(" -> handle ");
			dbg_hex(*(uint32_t *)a[1]);
		}
		dbg_puts(" err ");
		dbg_hex(r);
		dbg_puts("\n");
	}
	a[1] = r;
	return 1;
}

int probe_impl(uint32_t id, uint32_t *args, uint32_t r9)
{
	if (id == SLOT_ID(1, 0x9f)) {			/* KALTaskGetCurrentID(uint32_t *tidP) */
		uint32_t *tidP = (uint32_t *)args[1];

		args[1] = kal_task_get_current_id(tidP);
		dbg_puts("g4pre:   KALTaskGetCurrentID -> *tidP = ");
		dbg_hex(tidP ? *tidP : 0);
		dbg_puts(" (temporary ID rule: registration order)\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x0b) || id == SLOT_ID(1, 0x0d)) {	/* HALDisplayGetAttributes / HALDisplaySetAttributes */
		uint32_t attr = args[1] & 0xffff, err, ok;
		const int get = id == SLOT_ID(1, 0x0b);

		ok = get ? hal_display_get_attr(attr, (void *)args[2], &err) : hal_display_set_attr(attr, args[2], &err);
		dbg_puts(get ? "g4d:   HALDisplayGetAttributes " : "g4d:   HALDisplaySetAttributes ");
		dbg_hex(attr);
		if (!ok) {
			dbg_puts(" not implemented in this DAL -> stop\n");
			return 0;
		}
		dbg_puts(get ? " -> value " : " value ");
		if (get && !err) {		/* 16-bit attributes (0x1f..0x21) may sit at a 2-byte-aligned address */
			const uint8_t *v = (const uint8_t *)args[2];

			dbg_hex((attr >= 0x1f && attr <= 0x21) ? (uint32_t)(v[0] | (v[1] << 8)) :
				(uint32_t)(v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24)));
		} else
			dbg_hex(get ? 0 : args[2]);
		dbg_puts(" err ");
		dbg_hex(err);
		dbg_puts("\n");
		args[1] = err;
		return 1;
	}
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && (id & 0xfff) >= 0x92 && (id & 0xfff) <= 0x9a && (id & 0xfff) != 0x96) {
		/* KAL mutexes 0x92..0x95, semaphores 0x97..0x9a (g4g) */
		static const char *const names[] = { "MutexCreate", "MutexDelete", "MutexRelease", "MutexReserve", "",
			"SemaphoreCreate", "SemaphoreDelete", "SemaphoreSignal", "SemaphoreWait" };
		uint32_t s = id & 0xfff, r;

		switch (s) {
		case 0x92: r = kal_mutex_create((uint32_t *)args[1], args[2]); break;
		case 0x93: r = kal_mutex_delete(args[1]); break;
		case 0x94: r = kal_mutex_release(args[1]); break;
		case 0x95: r = kal_mutex_reserve(args[1], (int32_t)args[2]); break;
		case 0x97: r = kal_sem_create((uint32_t *)args[1], args[2], args[3]); break;
		case 0x98: r = kal_sem_delete(args[1]); break;
		case 0x99: r = kal_sem_signal(args[1]); break;
		default: r = kal_sem_wait(args[1], (int32_t)args[2]); break;
		}
		dbg_puts("g4g:   KAL");
		dbg_puts(names[s - 0x92]);
		dbg_puts(" ");
		dbg_hex(args[1]);
		if (s == 0x92 || s == 0x97) {
			dbg_puts(" tag ");
			dbg_hex(args[2]);
			if (!r) {
				dbg_puts(" -> handle ");
				dbg_hex(*(uint32_t *)args[1]);
			}
		}
		dbg_puts(" err ");
		dbg_hex(r);
		dbg_puts("\n");
		args[1] = r;
		return 1;
	}
	if (id == SLOT_ID(1, 0xa4)) {			/* KALTaskSwitching(Boolean enable) */
		uint32_t en = args[1] & 0xff;

		args[1] = kal_task_switching(en);
		dbg_puts(en ? "g4g:   KALTaskSwitching(on) err " : "g4g:   KALTaskSwitching(off) err ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		return 1;
	}
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && hal_misc_dispatch(id & 0xfff, args, r9)) {
		uint32_t sl = id & 0xfff;

		if (sl != 0x02 && sl != 0xcc && (sl < 0x7a || sl > 0x7e)) {	/* debug output already logged */
			dbg_puts("g4j:   misc HAL slot ");
			dbg_hex(sl);
			dbg_puts(" -> ");
			dbg_hex(args[1]);
			dbg_puts("\n");
		}
		return 1;
	}
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && (id & 0xfff) >= 0x20 && (id & 0xfff) <= 0x43)	/* HALFlp* (g4n) */
		return hal_flp_dispatch(id & 0xfff, args);
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && kal_dispatch(id & 0xfff, args, r9))			/* remaining KAL (g4m) */
		return 1;
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && (id & 0xfff) >= 0xad && (id & 0xfff) <= 0xb6)	/* RAL (g4k) */
		return ral_dispatch(id & 0xfff, args, r9);
	if (id == SLOT_ID(1, 0x16) || id == SLOT_ID(1, 0xbf)) {	/* HALDrawFindIndexes(count, records, tableP) */
		args[1] = hal_draw_find_indexes(args[1], (uint8_t *)args[2], (const void *)args[3]);
		return 1;
	}
	if (id == SLOT_ID(1, 0xcd)) {			/* HALDraw_GetSupportedDensity(u16 *densityP) */
		static const uint16_t seq[] = { 0, 72, 108, 144, 216, 288, 0 };
		uint16_t *d = (uint16_t *)args[1];
		uint32_t i;

		uint32_t in = *d;

		args[1] = 0xffff;
		for (i = 0; i < 6; i++)
			if (*d == seq[i]) {
				*d = seq[i + 1];
				args[1] = 0;
				break;
			}
		dbg_puts("g4o:   HALDraw_GetSupportedDensity ");
		dbg_hex(in);
		dbg_puts(" -> ");
		dbg_hex(*d);
		dbg_puts(" err ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		return 1;
	}
	if ((id & ~0xfffu) == SLOT_ID(1, 0) && (id & 0xfff) >= 0xbe && (id & 0xfff) <= 0xc4 && (id & 0xfff) != 0xbf) {
		/* g4r: HALDraw rendering into 16 bpp destinations (g4pre/hal_draw.c); stack arguments from args[6] on */
		switch (id & 0xfff) {
		case 0xbe:	/* Bitmap(canvas, src, rect, xOff, yOff) */
			args[1] = hal_draw_bitmap((const uint8_t *)args[1], (const uint8_t *)args[2], (const int16_t *)args[3],
						  (int32_t)args[4], (int32_t)args[6]);
			break;
		case 0xc0:	/* GetPixel(bmp, x, y, asIndex) */
			args[1] = hal_draw_get_pixel((const uint8_t *)args[1], (int16_t)args[2], (int16_t)args[3], args[4] & 0xff);
			break;
		case 0xc1:	/* Chars(canvas, x, y, chars, len, font, fontMap, checkCb) */
			hal_draw_chars((const uint8_t *)args[1], (int16_t)args[2], (int16_t)args[3], (const uint8_t *)args[4],
				       (int16_t)args[6], (const uint8_t *)args[7], (const uint8_t *)args[8], args[9], r9);
			break;
		case 0xc2:	/* Line(canvas, x1, y1, x2, y2, penW) */
			hal_draw_line((const uint8_t *)args[1], (int32_t)args[2], (int32_t)args[3], (int32_t)args[4],
				      (int32_t)args[6], (int32_t)args[7]);
			break;
		case 0xc3:	/* Rectangle(canvas, rect, radius, penW) */
			hal_draw_rectangle((const uint8_t *)args[1], (const int16_t *)args[2], (int32_t)args[3], (int32_t)args[4]);
			break;
		default:	/* Pixels(canvas, n, points, penW) */
			hal_draw_pixels((const uint8_t *)args[1], args[2], (const int16_t *)args[3], (int32_t)args[4]);
			break;
		}
		return 1;
	}
	if (id == SLOT_ID(1, 0x67)) {			/* HALScreenInit(outP, paletteP) */
		uint32_t *outP = (uint32_t *)args[1];

		args[1] = hal_screen_init(outP, (const void *)args[2]);
		dbg_puts("g4l:   HALScreenInit -> descriptor ");
		dbg_hex(outP ? *outP : 0);
		dbg_puts(" palette given ");
		dbg_hex(args[2] != 0);
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x6c)) {			/* HALScreenUpdateBitmap(depth) */
		dbg_puts("g4l:   HALScreenUpdateBitmap ");
		dbg_hex(args[1] & 0xff);
		dbg_puts("\n");
		args[1] = hal_screen_update_bitmap(args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x66)) {			/* HALScreenGetColortable() */
		args[1] = hal_screen_get_colortable();
		return 1;
	}
	if (id == SLOT_ID(1, 0x69)) {			/* HALScreenPalette(start, count, depth, tableP, palettes) */
		args[1] = hal_screen_palette((int32_t)args[1], args[2], args[3], (const void *)args[4],
					     *(const void *const **)&args[6]);
		dbg_puts("g4l:   HALScreenPalette\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x64)) {			/* HALScreenDefaultPalette() */
		args[1] = hal_screen_default_palette();
		return 1;
	}
	if (id == SLOT_ID(1, 0x68)) {			/* HALScreenLock(mode) */
		int fatal;

		args[1] = hal_screen_lock(args[1], &fatal);
		if (fatal) {
			dbg_puts("g4l:   HALScreenLock: lock overflow or invalid mode (fatal)\n");
			return 0;
		}
		return 1;
	}
	if (id == SLOT_ID(1, 0x6b)) {			/* HALScreenUnlock() */
		args[1] = hal_screen_unlock();
		return 1;
	}
	if (id == SLOT_ID(1, 0x57) || id == SLOT_ID(1, 0x5a)) {	/* HALPenRawToScreen / HALPenScreenToRaw(PointType *) */
		args[1] = id == SLOT_ID(1, 0x57) ? hal_pen_raw_to_screen((int16_t *)args[1]) : hal_pen_screen_to_raw((int16_t *)args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x56)) {			/* HALPenCalibrate(digTL, digBR, scrTL, scrBR) */
		uint32_t err, dens = 72;

		uint32_t stage = hal_get_init_stage();

		/* g4w: the reference refuses calibration before init stage 3 (measured: after HALSetInitStage(2) it
		 * returns 0xFFFF and the maps stay unchanged, after HALSetInitStage(3) it accepts); the OS makes one
		 * such call during boot, after HALPenResetCalibration */
		if (stage == 0xffffffffu || stage < 3) {
			args[1] = 0xffff;
			dbg_puts("g4w:   HALPenCalibrate before init stage 3 -> 0xffff, maps unchanged\n");
			return 1;
		}
		hal_display_get_attr(0x07, &dens, &err);
		args[1] = hal_pen_calibrate((const int16_t *)args[1], (const int16_t *)args[2], (const int16_t *)args[3],
					    (const int16_t *)args[4], dens);
		dbg_puts("g4q:   HALPenCalibrate -> ");
		dbg_hex(args[1]);
		dbg_puts(" (not persisted)\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x58)) {			/* HALPenResetCalibration() */
		args[1] = hal_pen_reset_calibration();
		dbg_puts("g4q:   HALPenResetCalibration (not persisted)\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x72)) {			/* HALTimeGetAlarm(secondsP) */
		args[1] = hal_time_get_alarm((uint32_t *)args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x76)) {			/* HALTimeSetAlarm(seconds) */
		dbg_puts("g4o:   HALTimeSetAlarm ");
		dbg_hex(args[1]);
		dbg_puts(" (now ");
		dbg_hex(hal_time_get_seconds());
		dbg_puts(")\n");
		args[1] = hal_time_set_alarm(args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x1d)) {			/* HALErrDisplay(msg, ...): fatal OS error -> log the text, then stop */
		const char *m = (const char *)args[1];
		char c[2] = { 0, 0 };
		uint32_t i;

		dbg_puts("g4j: HALErrDisplay \"");
		if (args[1] >= 0x10000000u && args[1] < 0x22000000u)
			for (i = 0; i < 160 && m[i]; i++) {
				c[0] = (m[i] >= 0x20 && m[i] < 0x7f) ? m[i] : '.';
				dbg_puts(c);
			}
		dbg_puts("\" (fatal error reported by the OS)\n");
		return 0;
	}
	if (id == SLOT_ID(1, 0x6d)) {			/* HALSetInitStage(stage): all stages (g4j), replaces the g2e replay */
		check_boot_block();
		args[1] = hal_set_init_stage(args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x1f)) {			/* HALEventRegisterCallBack(type, cb, oldP) */
		dbg_puts("g4j:   HALEventRegisterCallBack type ");
		dbg_hex(args[1]);
		dbg_puts(" cb ");
		dbg_hex(args[2]);
		dbg_puts("\n");
		args[1] = hal_event_register_callback(args[1], args[2], (uint32_t *)args[3], r9);
		return 1;
	}
	if (id == SLOT_ID(1, 0x1e)) {			/* HALEventPost(type, data) */
		args[1] = hal_event_post(args[1], (const void *)args[2]);
		dbg_puts("g4j:   HALEventPost\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x53)) {			/* HALMemorySetStorageAreaProtectionState(Boolean writable) */
		volatile uint32_t *probe = (volatile uint32_t *)(PVMEM_STOR_BASE + PVMEM_STOR_SIZE - 4);
		uint32_t before = hal_memory_get_storage_protection(), old = *probe, ok;

		static uint32_t checks;

		hal_memory_set_storage_protection(args[1]);
		ok = 1;
		if (checks < 8) {				/* write check (first calls only): writes are never blocked */
			checks++;
			*probe = 0x5a5aa5a5u;
			ok = *probe == 0x5a5aa5a5u;
			*probe = old;
		}
		dbg_puts("g4i:   HALMemorySetStorageAreaProtectionState(");
		dbg_hex(args[1] & 0xff);
		dbg_puts(") state ");
		dbg_hex(before);
		dbg_puts(" -> ");
		dbg_hex(hal_memory_get_storage_protection());
		dbg_puts(ok ? ", storage write check ok (not enforced)\n" : ", storage write check FAILED\n");
		args[1] = 0;	/* g4w: no return value in the spec; r0 is 0 after every call on the reference (measured) */
		return 1;
	}
	if (id == SLOT_ID(1, 0x52)) {			/* HALMemoryGetStorageAreaProtectionState(void) */
		args[1] = hal_memory_get_storage_protection();
		dbg_puts("g4i:   HALMemoryGetStorageAreaProtectionState -> ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x55)) {			/* HALMemoryGetAreaSize(addr, access) */
		dbg_puts("g4i:   HALMemoryGetAreaSize ");
		dbg_hex(args[1]);
		args[1] = hal_memory_get_area_size(args[1]);
		dbg_puts(" -> ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x84)) {			/* HALDisplayDrawBootScreen(x, y, bmpP) (g4s; was a bypass) */
		args[1] = hal_display_draw_boot_screen((int16_t)args[1], (int16_t)args[2], (const uint8_t *)args[3]);
		dbg_puts("g4s: HALDisplayDrawBootScreen drawn\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x62)) {			/* HALRandomInitializeSeed(UInt32 *randomSeedP) */
		uint32_t *seedP = (uint32_t *)args[1];

		args[1] = hal_random_initialize_seed(seedP);
		dbg_puts("g4f:   HALRandomInitializeSeed -> *seedP = ");
		dbg_hex(*seedP);
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x61)) {			/* HALRandomGetBits(numBits, bitsP) */
		args[1] = hal_random_get_bits(args[1], (void *)args[2]);
		dbg_puts("g4f:   HALRandomGetBits\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x73)) {			/* HALTimeGetSeconds(void) */
		args[1] = hal_time_get_seconds();
		dbg_puts("g4f:   HALTimeGetSeconds -> ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x77)) {			/* HALTimeSetSeconds(seconds) */
		args[1] = hal_time_set_seconds(args[1]);
		dbg_puts("g4f:   HALTimeSetSeconds\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x74)) {			/* HALTimeGetSystemTime(void) -> ms */
		args[1] = hal_time_get_system_time();
		return 1;					/* frequent: not logged */
	}
	if (id == SLOT_ID(1, 0x0a)) {			/* HALDelay(microseconds) */
		dbg_puts("g4f:   HALDelay ");
		dbg_hex(args[1]);
		dbg_puts("\n");
		args[1] = hal_delay(args[1]);
		return 1;
	}
	if (id == SLOT_ID(1, 0x4d)) {			/* HALKeyGetState(void) -> key bits */
		args[1] = hal_key_get_state();
		dbg_puts("g4e:   HALKeyGetState -> ");
		dbg_hex(args[1]);
		dbg_puts(" (direct register read, input driver not brought up)\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x4e)) {			/* HALKeyResetDoubleTap(delayP): no double-tap timing kept (g4r) */
		args[1] = 0;
		return 1;
	}
	if (id == SLOT_ID(1, 0x4f)) {			/* HALKeySetMask(mask) -> previous mask */
		args[1] = hal_key_set_mask(args[1]);
		dbg_puts("g4e:   HALKeySetMask\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x4c)) {			/* HALKeyGetRates(initDelayP, periodP, queueAheadP) */
		args[1] = hal_key_get_rates((uint16_t *)args[1], (uint16_t *)args[2], (uint8_t *)args[3]);
		dbg_puts("g4e:   HALKeyGetRates\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x50)) {			/* HALKeySetRates(initDelay, period, queueAhead) */
		args[1] = hal_key_set_rates((uint16_t)args[1], (uint16_t)args[2], (uint8_t)args[3]);
		dbg_puts("g4e:   HALKeySetRates\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x0e)) {			/* HALDisplaySetPalette(start, count, entries) */
		args[1] = hal_display_set_palette((int32_t)args[1], args[2], (const void *)args[3]);
		dbg_puts("g4d:   HALDisplaySetPalette\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0x0f) || id == SLOT_ID(1, 0x10) || id == SLOT_ID(1, 0x65) || id == SLOT_ID(1, 0x6a)) {
		/* HALDisplayDoze / HALDisplayWake / HALScreenDrawNotify / HALScreenSendUpdateArea: no PV hardware action */
		dbg_puts("g4d:   display call with no PV hardware action (slot ");
		dbg_hex(id & 0xfff);
		dbg_puts(")\n");
		args[1] = 0;
		return 1;
	}
	if (id == SLOT_ID(1, 0x51)) {			/* HALMemoryGetMemoryMap(void) */
		const uint8_t *m = hal_memory_get_memory_map();
		const uint8_t *e = *(const uint8_t * const *)(m + 4);
		uint32_t i;

		args[1] = (uint32_t)m;
		dbg_puts("g4b:   HALMemoryGetMemoryMap -> ");
		dbg_hex(args[1]);
		dbg_puts(" entries ");
		dbg_hex(m[0]);
		for (i = 0; i < m[0]; i++) {
			dbg_puts(i ? ", " : ": ");
			dbg_hex(e[12 * i]);
			dbg_puts(" ");
			dbg_hex(*(const uint32_t *)(e + 12 * i + 4));
			dbg_puts("+");
			dbg_hex(*(const uint32_t *)(e + 12 * i + 8));
		}
		dbg_puts("\n");
		return 1;
	}
	if (id == SLOT_ID(1, 0xa0)) {			/* KALTaskGetInfo(uint32_t tid, info) */
		uint8_t *info = (uint8_t *)args[2], before[36];
		uint32_t i, changed = 0, outside = 0;

		if (info)
			memcpy(before, info, sizeof(before));
		args[1] = kal_task_get_info(args[1], info, (uint32_t)&args[6], args[5]);
		if (info)
			for (i = 0; i < sizeof(before); i++)
				if (info[i] != before[i]) {
					changed++;
					if (i >= 0x18)
						outside++;
				}
		dbg_puts("g4a:   KALTaskGetInfo -> +04 ");
		dbg_hex(info ? *(uint32_t *)(info + 4) : 0);
		dbg_puts(" +08 ");
		dbg_hex(info ? *(uint32_t *)(info + 8) : 0);
		dbg_puts(" +0c ");
		dbg_hex(info ? *(uint32_t *)(info + 12) : 0);
		dbg_puts(" (+08 + +0c = ");
		dbg_hex(info ? *(uint32_t *)(info + 8) + *(uint32_t *)(info + 12) : 0);
		dbg_puts(") bytes changed ");
		dbg_hex(changed);
		dbg_puts(outside ? " WRITE OUTSIDE +00..+17\n" : ", none outside +00..+17\n");
		check_boot_block();
		if (info) {
			dbg_puts("g4a:     record after (+00..+17):");
			for (i = 0; i < 0x18; i += 4) {
				dbg_puts(" ");
				dbg_hex(*(uint32_t *)(info + i));
			}
			dbg_puts("\n");
		}
		return 1;
	}
	return 0;
}

static TaskHandle_t gBootTask;
static uint32_t gBootTaskData[8] __attribute__((aligned(8)));	/* boot task user data: 8 bytes zero + 24 sentinel bytes */
#define BOOT_DATA_SENTINEL	0xb0075e17u

/* g4j: the reference block is 8 bytes; words 2..7 here are sentinels so an access beyond 8 bytes is reported */
static void check_boot_block(void)
{
	uint32_t i, bad = 0;

	for (i = 2; i < 8; i++)
		if (gBootTaskData[i] != BOOT_DATA_SENTINEL)
			bad++;
	if (bad) {
		dbg_puts("g4j: boot task user-data block written beyond 8 bytes (");
		dbg_hex(bad);
		dbg_puts(" words)\n");
	}
}

static void boot_task(void *arg)
{
	uint32_t exp0 = probe_export0_addr(), ret, tid = 0;
	(void)arg;

	kal_task_get_current_id(&tid);
#if DAL_SELFTEST
	selftest_draw();
#if DAL_LOG
	selftest_stop_context();
#endif
#endif
	dbg_puts("g4pre: boot task running, KAL id ");
	dbg_hex(tid);
	dbg_puts(", calling export 0 at ");
	dbg_hex(exp0);
	dbg_puts(" r0..r3 = 0, flags 0x4\n");
	ret = probe_call_in_task(exp0, probe_r9(), 0x40000000u);
	dbg_puts("g4pre: export 0 returned ");
	dbg_hex(ret);
	dbg_puts("\n");
	vTaskSuspend(NULL);
}

int main(void)
{
	uint32_t ret;

	dbg_puts("g4pre: FreeRTOS " tskKERNEL_VERSION_NUMBER " + Boot probe\n");
	pic_init();
	/* g4b: the reported RAM regions end at 0x22000000, so the PV session must provide at least 32 MiB */
	if (PV_SYSCTL_RAMSIZE < PVMEM_RAM_REQUIRED) {
		dbg_puts("g4b: PV RAM ");
		dbg_hex(PV_SYSCTL_RAMSIZE);
		dbg_puts(" < required 0x02000000, halting\n");
		for (;;) ;
	}
	{
		extern uint8_t __stack_sys_top[];
		dbg_puts("g4b: PV RAM ");
		dbg_hex(PV_SYSCTL_RAMSIZE);
		dbg_puts(", DAL RAM ends at ");
		dbg_hex((uint32_t)__stack_sys_top);
		dbg_puts((uint32_t)__stack_sys_top <= PVMEM_FB_BASE ? " (below 0x20100000, ok)\n" : " (OVERLAPS reserved area)\n");
	}
#ifdef G4_INPUT_TEST
	/* test build only (make ... G4_DEFS=-DG4_INPUT_TEST): give the emulator's scripted key press time to land
	 * before Boot runs, so HALKeyGetState can be checked with a key held */
	{
		uint32_t t0 = PV_TIMER_LO;
		while (PV_TIMER_LO - t0 < 3000)
			;
		dbg_puts("g4e: input test, key register now ");
		dbg_hex(PV_KEYS_STATE);
		dbg_puts("\n");
	}
#endif
	hal_random_init();
	hal_display_init();
	dbg_puts("g4d: PV display spec ");
	dbg_hex(PV_DISP_SPEC);
	dbg_puts(", framebuffer 0x20100000 16 bpp, stride ");
	dbg_hex(PV_DISP_STRIDE);
	dbg_puts("\n");
	if (probe_prepare())
		for (;;) ;
	if (ral_init()) {
		dbg_puts("g4pre: DAL patches on Boot failed\n");
		for (;;) ;
	}
	ret = probe_cmd_7ffe();
	if (ret) {
		dbg_puts("g4pre: 0x7ffe failed\n");
		for (;;) ;
	}
	PV_IC_MASK = 0;					/* the scheduler enables the tick line itself */
	if (xTaskCreate(boot_task, "boot", 2048, NULL, kal_prio_map(100), &gBootTask) != pdPASS)
		for (;;) ;
	dbg_puts("g4pre: boot task registered as KAL id ");
	/* g4i-fix: the task that runs Boot export 0 gets a user-data word pointing to a dedicated, zero-filled, writable
	 * block that is never freed (Boot writes through it; the reference works with 8 bytes, 32 are reserved here),
	 * Palm priority 100 and a diagnostic tag */
	{
		uint32_t i;

		for (i = 2; i < 8; i++)
			gBootTaskData[i] = BOOT_DATA_SENTINEL;
	}
	dbg_hex(kal_task_register_attrs(gBootTask, (uint32_t)gBootTaskData, 100, 0x70766274u /* 'pvbt' */));
	dbg_puts("\n");
	vTaskStartScheduler();
	dbg_puts("g4pre: scheduler returned\n");
	for (;;) ;
}
