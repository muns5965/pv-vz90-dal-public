/* pv-vz90-dal g4f: random numbers and time on the PV RTC and 1 kHz timer. Requirements: local functional spec
 * (analyst role) + PV device facts (CloudpilotEmu pv_rtc.cpp, pv_timer.cpp); see docs/INTERFACE.md §18.
 * The generator is a plain xorshift32 (a generic public-domain construction); the sequence is not meant to match
 * the reference. It is seeded at start-up from both the wall clock and the millisecond uptime, so runs differ.
 * Not cryptographically strong.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_time.h"
#include "../src/pv_hw.h"

static uint32_t gRng = 1;
static int32_t gTimeOffset;		/* device time = RTC + offset (lost on reset) */

static uint32_t rng_next(void)
{
	uint32_t x = gRng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	gRng = x;
	return x;
}

void hal_random_init(void)
{
	uint32_t s = PV_RTC_SECONDS * 0x9e3779b9u ^ PV_TIMER_LO ^ (PV_TIMER_HI << 16);
	int i;

	gRng = s ? s : 0x6d2b79f5u;
	for (i = 0; i < 8; i++)
		rng_next();
}

uint32_t hal_random_get_bits(uint32_t numBits, void *bitsP)
{
	uint8_t *p = bitsP;
	uint32_t n = (numBits + 7) / 8, v = 0, i;

	for (i = 0; i < n; i++) {
		if (!(i & 3))
			v = rng_next();
		p[i] = (uint8_t)v;
		v >>= 8;
	}
	return 0;
}

uint32_t hal_random_initialize_seed(uint32_t *randomSeedP)
{
	*randomSeedP = rng_next();
	return 0;
}

uint32_t hal_time_get_seconds(void)
{
	return PV_RTC_SECONDS + (uint32_t)gTimeOffset;
}

uint32_t hal_time_set_seconds(uint32_t seconds)
{
	gTimeOffset = (int32_t)(seconds - PV_RTC_SECONDS);
	return 0;
}

uint32_t hal_time_get_system_time(void)
{
	return PV_TIMER_LO;
}

/* g4r: HALDelay, written anew from the requirement only (local spec HALRandomTime): wait at least the requested
 * time on the 1 kHz PV counter without yielding. The wait in milliseconds is the request rounded up plus one tick
 * for the partly elapsed current tick; requests are capped at 2 s. If the counter does not advance, the loop ends
 * after a fixed budget of counter reads per requested millisecond instead of hanging. */
#define DELAY_CAP_US		2000000u
#define DELAY_READS_PER_MS	200000u

uint32_t hal_delay(uint32_t microseconds)
{
	uint32_t want_ms, begin, budget;

	if (microseconds == 0)
		return 0;
	if (microseconds > DELAY_CAP_US)
		microseconds = DELAY_CAP_US;
	want_ms = microseconds / 1000u + (microseconds % 1000u ? 1u : 0u) + 1u;
	begin = PV_TIMER_LO;
	for (budget = want_ms * DELAY_READS_PER_MS; budget; budget--)
		if ((uint32_t)(PV_TIMER_LO - begin) >= want_ms)
			break;
	return 0;
}
