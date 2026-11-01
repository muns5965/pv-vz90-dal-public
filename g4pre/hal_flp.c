/* pv-vz90-dal g4n: HALFlp* (slots 0x20..0x43): IEEE-754 single/double arithmetic, comparison and conversion with the
 * public FlpXxx meanings, soft-float register passing (float in r0, double in r0:r1 low word first, second operand in
 * r2:r3, 64-bit results in r0:r1, Boolean 0/1). Computed with the compiler's soft-float support (libgcc).
 * Long-double slots (0x2a, 0x30, 0x36, 0x37) are not supported (the probe stops there).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "hal_flp.h"

union d { double v; uint64_t u; };
union f { float v; uint32_t u; };

static double D(uint32_t lo, uint32_t hi) { union d x; x.u = ((uint64_t)hi << 32) | lo; return x.v; }
static float F(uint32_t w) { union f x; x.u = w; return x.v; }
static void retD(uint32_t *a, double v) { union d x; x.v = v; a[1] = (uint32_t)x.u; a[2] = (uint32_t)(x.u >> 32); }
static void retF(uint32_t *a, float v) { union f x; x.v = v; a[1] = x.u; }
static void ret64(uint32_t *a, uint64_t v) { a[1] = (uint32_t)v; a[2] = (uint32_t)(v >> 32); }

static int32_t to_i32(double v) { return v != v ? 0 : v >= 2147483647.0 ? 0x7fffffff : v <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)v; }
static uint32_t to_u32(double v) { return !(v > 0) ? 0 : v >= 4294967295.0 ? 0xffffffffu : (uint32_t)v; }
static int64_t to_i64(double v) { return v != v ? 0 : v >= 9.2233720368547758e18 ? INT64_MAX : v <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)v; }
static uint64_t to_u64(double v) { return !(v > 0) ? 0 : v >= 1.8446744073709552e19 ? UINT64_MAX : (uint64_t)v; }

int hal_flp_dispatch(uint32_t s, uint32_t *a)
{
	uint64_t ll = ((uint64_t)a[2] << 32) | a[1];

	switch (s) {
	case 0x20: retD(a, D(a[1], a[2]) + D(a[3], a[4])); break;
	case 0x21: retF(a, F(a[1]) + F(a[2])); break;
	case 0x22: a[1] = D(a[1], a[2]) == D(a[3], a[4]); break;
	case 0x23: a[1] = D(a[1], a[2]) < D(a[3], a[4]); break;
	case 0x24: a[1] = F(a[1]) == F(a[2]); break;
	case 0x25: a[1] = F(a[1]) < F(a[2]); break;
	case 0x26: retD(a, D(a[1], a[2]) / D(a[3], a[4])); break;
	case 0x27: retF(a, F(a[1]) / F(a[2])); break;
	case 0x28: retF(a, (float)D(a[1], a[2])); break;
	case 0x29: a[1] = (uint32_t)to_i32(D(a[1], a[2])); break;
	case 0x2b: ret64(a, (uint64_t)to_i64(D(a[1], a[2]))); break;
	case 0x2c: a[1] = to_u32(D(a[1], a[2])); break;
	case 0x2d: ret64(a, to_u64(D(a[1], a[2]))); break;
	case 0x2e: retD(a, (double)F(a[1])); break;
	case 0x2f: a[1] = (uint32_t)to_i32(F(a[1])); break;
	case 0x31: ret64(a, (uint64_t)to_i64(F(a[1]))); break;
	case 0x32: a[1] = to_u32(F(a[1])); break;
	case 0x33: ret64(a, to_u64(F(a[1]))); break;
	case 0x34: retD(a, (double)(int32_t)a[1]); break;
	case 0x35: retF(a, (float)(int32_t)a[1]); break;
	case 0x38: retD(a, (double)(int64_t)ll); break;
	case 0x39: retF(a, (float)(int64_t)ll); break;
	case 0x3a: retD(a, D(a[1], a[2]) * D(a[3], a[4])); break;
	case 0x3b: retF(a, F(a[1]) * F(a[2])); break;
	case 0x3c: retD(a, -D(a[1], a[2])); break;
	case 0x3d: retF(a, -F(a[1])); break;
	case 0x3e: retD(a, D(a[1], a[2]) - D(a[3], a[4])); break;
	case 0x3f: retF(a, F(a[1]) - F(a[2])); break;
	case 0x40: retD(a, (double)a[1]); break;
	case 0x41: retF(a, (float)a[1]); break;
	case 0x42: retD(a, (double)ll); break;
	case 0x43: retF(a, (float)ll); break;
	default: return 0;
	}
	return 1;
}
