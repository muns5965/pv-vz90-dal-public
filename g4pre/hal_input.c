/* pv-vz90-dal g4e: key input on the PV key device. Requirements: local functional spec (analyst role) + PV device
 * facts (CloudpilotEmu pv_keys.cpp); see docs/INTERFACE.md §17.
 * Stage g4e implements the state used before the input driver is brought up: HALKeyGetState reads the key register
 * directly. It only reads (a write would clear the key IRQ). The interrupt-driven path (IRQ 3/1, event queue,
 * tracked state, auto-repeat) is not implemented yet.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_input.h"
#include "hal_events.h"
#include "../src/pv_hw.h"

#define PV_KEY_SELECT		0x08000000u
#define KEYBIT_NAV_SELECT	0x04000000u		/* Palm OS keyBitNavSelect */

static uint32_t gKeyMask = 0xffffffffu;
static uint16_t gInitDelay, gPeriod;
static uint8_t gQueueAhead = 1;

/* PV key word -> Palm OS key bits (identical except nav select) */
static uint32_t map_keys(uint32_t pv)
{
	return (pv & ~PV_KEY_SELECT) | ((pv & PV_KEY_SELECT) ? KEYBIT_NAV_SELECT : 0);
}

uint32_t hal_key_get_state(void)
{
	/* before the input driver is up: direct register read; afterwards the state tracked from queued changes (g4j) */
	return hal_input_driver_up() ? hal_input_tracked_keys() : map_keys(PV_KEYS_STATE);
}

uint32_t hal_key_set_mask(uint32_t mask)
{
	uint32_t old = gKeyMask;

	gKeyMask = mask;
	return old;
}

uint32_t hal_key_get_rates(uint16_t *initDelayP, uint16_t *periodP, uint8_t *queueAheadP)
{
	if (initDelayP)
		*initDelayP = gInitDelay;
	if (periodP)
		*periodP = gPeriod;
	if (queueAheadP)
		*queueAheadP = gQueueAhead;
	return 0;
}

uint32_t hal_key_set_rates(uint16_t initDelay, uint16_t period, uint8_t queueAhead)
{
	gInitDelay = initDelay;
	gPeriod = period;
	gQueueAhead = queueAhead ? 1 : 0;
	return 0;
}

/* ---- g4q: pen calibration (local spec HALInput "Pen calibration model"): per axis a forward (raw -> screen) and
 * a reverse (screen -> raw) map, each a 16.16 slope plus an integer offset, applied as trunc(slope*v/65536) + offset;
 * identity by default (PV touch samples are native screen pixels). Not persisted yet (the reference keeps the maps
 * in a private system preference; recorded as a difference). */
struct pen_map { int32_t slope, offset; };
static struct pen_map gFwd[2] = { { 65536, 0 }, { 65536, 0 } }, gRev[2] = { { 65536, 0 }, { 65536, 0 } };

static int16_t pen_apply(const struct pen_map *m, int32_t v)
{
	return (int16_t)((int32_t)(((int64_t)m->slope * v) / 65536) + m->offset);
}

uint32_t hal_pen_raw_to_screen(int16_t *pt)
{
	pt[0] = pen_apply(&gFwd[0], pt[0]);
	pt[1] = pen_apply(&gFwd[1], pt[1]);
	return 0;
}

uint32_t hal_pen_screen_to_raw(int16_t *pt)
{
	pt[0] = pen_apply(&gRev[0], pt[0]);
	pt[1] = pen_apply(&gRev[1], pt[1]);
	return 0;
}

static int32_t slope_up(int32_t num, int32_t den)		/* 16.16 slope num/den, rounded up */
{
	int64_t n = (int64_t)num * 65536;

	return (int32_t)(n / den + ((n % den) && ((n > 0) == (den > 0)) ? 1 : 0));
}

uint32_t hal_pen_calibrate(const int16_t *digTL, const int16_t *digBR, const int16_t *scrTL, const int16_t *scrBR,
			   uint32_t density)
{
	struct pen_map f[2], r[2];
	int k;

	for (k = 0; k < 2; k++) {
		int32_t d1 = (digTL[k] * (int32_t)density + 36) / 72, d2 = (digBR[k] * (int32_t)density + 36) / 72;
		int32_t s1 = (scrTL[k] * (int32_t)density + 36) / 72, s2 = (scrBR[k] * (int32_t)density + 36) / 72;
		int32_t r1 = pen_apply(&gRev[k], d1), r2 = pen_apply(&gRev[k], d2);

		if (r1 == r2 || s1 == s2)
			return 0xffff;
		f[k].slope = slope_up(s2 - s1, r2 - r1);
		f[k].offset = s1 - (int32_t)(((int64_t)f[k].slope * r1) / 65536);
		r[k].slope = slope_up(r2 - r1, s2 - s1);
		r[k].offset = r1 - (int32_t)(((int64_t)r[k].slope * s1) / 65536);
	}
	for (k = 0; k < 2; k++) {
		gFwd[k] = f[k];
		gRev[k] = r[k];
	}
	return 0;
}

uint32_t hal_pen_reset_calibration(void)
{
	int k;

	for (k = 0; k < 2; k++) {
		gFwd[k].slope = gRev[k].slope = 65536;
		gFwd[k].offset = gRev[k].offset = 0;
	}
	return 0;
}

uint32_t hal_key_get_mask(void)
{
	return gKeyMask;
}
