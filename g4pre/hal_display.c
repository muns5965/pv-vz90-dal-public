/* pv-vz90-dal: PV display HAL (attributes, CLUT, screen bitmap, lock, colour lookup). SPDX-License-Identifier: GPL-3.0-or-later */
/* Clean-room implementation written from HALDisplay.md, HALScreen.md, HALDrawIface-s1.md and SUPPLEMENT.md
 * (SUPPLEMENT wins on conflicts); register semantics from pv_display.cpp. Section references: [D] HALDisplay.md,
 * [S] HALScreen.md, [F] HALDrawIface-s1.md, [X] SUPPLEMENT.md. */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hal_display.h"
#include "hal_memory.h"
#include "../src/pv_hw.h"

#define DAL_COLOR_REFERENCE		0
#define DAL_COLOR_SONY_NX70V	1
#ifndef DAL_COLOR_MODE
#define DAL_COLOR_MODE			DAL_COLOR_REFERENCE
#endif

#define VRAM_SIZE		0x00100000u				/* [D] PV device: 1 MiB VRAM at FB */
#define SONY_PAGE		0x3F843000u				/* [X] §0 */
#define ROM_MAGIC_ADDR	(PV_ROM_BASE + 0x100u)	/* [X] §2 */
#define ROM_MAGIC		0xFEEDFACEu

/* ---------------------------------------------------------------- objects owned by the DAL ([S] §1) */
struct sbd {				/* [S] §1a: BitmapTypeV3 + colour-table pointer + bits pointer, 32 bytes */
	uint16_t width;			/* 0x00 */
	uint16_t height;		/* 0x02 */
	uint16_t rowBytes;		/* 0x04 */
	uint16_t flags;			/* 0x06 */
	uint8_t pixelSize;		/* 0x08 */
	uint8_t version;		/* 0x09 */
	uint8_t size;			/* 0x0A */
	uint8_t pixelFormat;	/* 0x0B */
	uint8_t unused;			/* 0x0C */
	uint8_t compression;	/* 0x0D */
	uint16_t density;		/* 0x0E */
	uint32_t transparent;	/* 0x10 */
	uint32_t next;			/* 0x14 */
	uint32_t ctab;			/* 0x18 */
	uint32_t bits;			/* 0x1C */
};
_Static_assert(sizeof(struct sbd) == 32, "SBD must be 32 bytes");

struct cpo {				/* [S] §1b: ColorTableType with 256 slots */
	uint16_t count;
	uint16_t reserved;
	uint8_t e[256][4];		/* index, r, g, b */
};
_Static_assert(sizeof(struct cpo) == 1028, "CPO must be 1028 bytes");

#define FL_HASCTAB		0x0002u
#define FL_INDIRECT		0x0008u
#define FL_FORSCREEN	0x0010u
#define FL_DIRECT		0x0020u
#define FL_INDCTAB		0x0040u

static struct sbd gSbd __attribute__((aligned(4)));
static struct cpo gCpo __attribute__((aligned(4)));

/* ---------------------------------------------------------------- display state ([D] init, [X] §2) */
static uint32_t gW, gH, gDens;
static int gUsable;
static uint32_t gD = 16;			/* current depth (attr 0x0A) */
static uint32_t gBright = 255, gContrast = 128;
static uint32_t gLayout;			/* L, attr 0x8006 */
static uint32_t gUser;				/* U, attr 0x8000 */
static int gSonyPage;				/* shared page set up at init */

/* palette-standard state ([S] §6, [X] §5) */
enum { PAL_STANDARD, PAL_UNCHECKED, PAL_NONSTANDARD };
static int gPalState = PAL_STANDARD;
static uint8_t gStd[256][4];		/* private copy of the OS standard 8 bpp table ([X] §5) */
static int gStdValid;

/* private translation tables, 1/2/4/8 bpp ([S] §5 step 3) */
static uint8_t gXlat1[2], gXlat2[4], gXlat4[16], gXlat8[256];

/* screen lock ([S] §7, [X] §7) */
static uint32_t gLockCount;
static SemaphoreHandle_t gLockMtx;

/* ---------------------------------------------------------------- small helpers */
static void fill_bytes(uint32_t addr, uint8_t v, uint32_t n)
{
	volatile uint8_t *p = (volatile uint8_t *)addr;
	uint32_t w = v * 0x01010101u;

	while (n && ((uint32_t)p & 3)) {
		*p++ = v;
		n--;
	}
	while (n >= 4) {
		*(volatile uint32_t *)p = w;
		p += 4;
		n -= 4;
	}
	while (n--)
		*p++ = v;
}

static void copy_bytes(uint32_t dst, uint32_t src, uint32_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	const volatile uint8_t *s = (const volatile uint8_t *)src;

	if (!((dst | src) & 3)) {
		while (n >= 4) {
			*(volatile uint32_t *)d = *(const volatile uint32_t *)s;
			d += 4;
			s += 4;
			n -= 4;
		}
	}
	while (n--)
		*d++ = *s++;
}

static void put32(void *p, uint32_t v)		/* byte stores: valueP alignment is not guaranteed */
{
	uint8_t *b = p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static void put16(void *p, uint32_t v)
{
	uint8_t *b = p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static uint32_t tab_count(const uint8_t *t) { return t[0] | ((uint32_t)t[1] << 8); }

static int depth_code(uint32_t d)	/* DEPTH register code, -1 when unsupported ([D] PV device) */
{
	switch (d) {
	case 1: return 0;
	case 2: return 1;
	case 4: return 2;
	case 8: return 3;
	case 16: return 4;
	default: return -1;
	}
}

static void clut_put(const uint8_t *rec, uint32_t index)	/* [X] §4: index 31..24, R, G, B */
{
	PV_DISP_CLUT = ((index & 0xffu) << 24) | ((uint32_t)rec[1] << 16) | ((uint32_t)rec[2] << 8) | rec[3];
}

/* ---------------------------------------------------------------- initialisation ([D], [X] §2) */
void hal_display_init(void)
{
	const uint32_t spec = PV_DISP_SPEC;
	volatile uint8_t *pg = (volatile uint8_t *)SONY_PAGE;

	gW = ((spec & 0x1ffu) + 1) * 4;
	gH = (((spec >> 9) & 0x1ffu) + 1) * 4;
	gDens = (((spec >> 18) & 0x7u) + 1) * 36;
	gUsable = gW * gH * 2 <= VRAM_SIZE;
	if (!gUsable)
		return;

	gD = 16;
	gBright = 255;
	gContrast = 128;
	gLayout = gW > gH ? 1 : 0;
	gUser = 0;

	if (*(volatile uint32_t *)ROM_MAGIC_ADDR == ROM_MAGIC) {	/* full Palm OS ROM: Sony shared page present */
		pg[0x5D] = 0xE0;
		pg[0x5E] = 40;
		pg[0x5F] = 30;
		pg[0x6D] = 15;
		pg[0x70] = 0;
		pg[0x5C] = (uint8_t)(gBright * 95 / 255 + 5);
		*(volatile uint32_t *)(SONY_PAGE + 0x10) = 1;
		gSonyPage = 1;
	}

	fill_bytes(PVMEM_FB_BASE, 0xFF, gW * gH * 2);
	PV_DISP_DEPTH = 4;
	PV_DISP_STRIDE = gW * 2;
	PV_DISP_BASE = PVMEM_FB_BASE;		/* base last */
}

void hal_display_geometry(uint32_t *w, uint32_t *h, uint32_t *depth)
{
	if (w)
		*w = gUsable ? gW : 0;
	if (h)
		*h = gUsable ? gH : 0;
	if (depth)
		*depth = gD;
}

/* ---------------------------------------------------------------- attributes ([D] tables, [X] §1, §3) */
static const char gDriverName[] = "pv-vz90-dal PV display";

int hal_display_get_attr(uint32_t attr, void *valueP, uint32_t *err)
{
	uint32_t v;

	*err = 0;
	if (!gUsable)
		return 0;

	switch (attr) {
	case 0x03: v = (uint32_t)gDriverName; break;
	case 0x04: v = gW; break;
	case 0x05: v = gH; break;
	case 0x06: v = gW * gD / 8; break;
	case 0x07:
	case 0x08: v = gDens; break;
	case 0x09: v = 16; break;
	case 0x0A: v = gD; break;
	case 0x0B: v = 16; break;
	case 0x0C: v = 0x808B; break;
	case 0x0D: v = 1; break;
	case 0x11: v = 1; break;
	case 0x12: v = gBright; break;
	case 0x13: v = gContrast; break;
	case 0xF1: v = 0; break;
	case 0xE000: v = 72; break;
	case 0x19:
		*err = HALDISP_ERR_UNKNOWN_ATTR;
		return 1;
	case 0x1F:
		put16(valueP, gDens);
		return 1;
	case 0x20:
		put16(valueP, 64);
		return 1;
	case 0x21:
		put16(valueP, 64 * 72 / gDens);
		return 1;
	case 0x8001:
	case 0x8002:
		if (gD < 8)
			return 0;
		if (attr == 0x8001)
			v = (gLayout == 1 && (gUser & 4)) ? 30 : 320;
		else
			v = (gLayout == 1 && (gUser & 4)) ? 0 : 450;
		v = v * gD / 8;
		break;
	case 0x8003:
	case 0x8004:
		*err = HALDISP_ERR_REFUSED;
		return 1;
	case 0x8005: v = PVMEM_FB_BASE; break;
	case 0x8006: v = gLayout; break;
	case 0x8009:
		if (!gSonyPage)
			return 0;
		v = *(volatile uint8_t *)(SONY_PAGE + 0x70);
		break;
	default:
		return 0;
	}
	put32(valueP, v);
	return 1;
}

int hal_display_set_attr(uint32_t attr, uint32_t value, uint32_t *err)
{
	int code;

	*err = 0;
	if (!gUsable)
		return 0;

	switch (attr) {
	case 0x0A:
		code = depth_code(value);
		if (code < 0) {
			*err = HALDISP_ERR_REFUSED;
			return 1;
		}
		gD = value;
		PV_DISP_DEPTH = (uint32_t)code;
		PV_DISP_STRIDE = gW * value / 8;
		return 1;
	case 0x12:
		gBright = value > 255 ? 255 : value;
		return 1;
	case 0x13:
		gContrast = value > 255 ? 255 : value;
		return 1;
	case 0xF0:
	case 0x1A:
		return 1;
	case 0x8000:
		gUser = value & 7;
		return 1;
	case 0x8003:
	case 0x8004:
	case 0x8005:
		*err = HALDISP_ERR_REFUSED;
		return 1;
	case 0x8006:
		if (value == (gW > gH ? 1u : 0u))
			gLayout = value;
		else
			*err = HALDISP_ERR_REFUSED;
		return 1;
	default:
		return 0;
	}
}

/* ---------------------------------------------------------------- CLUT ([D] SetPalette, [X] §4) */
uint32_t hal_display_set_palette(int32_t start, uint32_t count, const void *entries)
{
	const uint8_t *rec = entries;
	uint32_t i;

	if (start == -1) {
		if (count > 256)
			return 0;
		for (i = 0; i < count; i++, rec += 4)
			clut_put(rec, rec[0]);
		return 0;
	}
	if (start < 0 || start > 255 || count > 256u - (uint32_t)start)
		return 0;
	for (i = 0; i < count; i++, rec += 4)
		clut_put(rec, (uint32_t)start + i);
	return 0;
}

/* ---------------------------------------------------------------- screen bitmap ([S] §2..§4, [X] §5) */
uint32_t hal_screen_update_bitmap(uint32_t depth)
{
	uint8_t fill;

	depth &= 0xff;
	if (!gUsable || depth == gSbd.pixelSize || depth_code(depth) < 0)
		return 0;

	gSbd.pixelSize = (uint8_t)depth;
	gSbd.rowBytes = (uint16_t)(gW * depth / 8);
	gSbd.flags |= FL_HASCTAB | FL_INDCTAB;
	if (depth == 16) {
		gSbd.flags |= FL_DIRECT;
		gSbd.pixelFormat = 2;		/* RGB565 LE */
		gCpo.count = 256;
		fill = 0xFF;
	} else {
		gSbd.flags &= (uint16_t)~FL_DIRECT;
		gSbd.pixelFormat = depth == 8 ? 0 : 3;	/* indexed / indexed LE */
		gCpo.count = (uint16_t)(1u << depth);
		fill = 0x00;
	}
	fill_bytes(PVMEM_FB_BASE, fill, gW * gH * depth / 8);
	return 0;
}

uint32_t hal_screen_init(uint32_t *outP, const void *paletteP)
{
	const uint8_t *pal = paletteP;

	if (outP)
		*outP = (uint32_t)&gSbd;
	if (!gUsable)
		return 0;

	/* step 1: descriptor */
	gSbd.width = (uint16_t)gW;
	gSbd.height = (uint16_t)gH;
	gSbd.density = (uint16_t)gDens;
	gSbd.version = 0x83;
	gSbd.size = 0x18;
	gSbd.flags |= FL_INDIRECT | FL_FORSCREEN;
	gSbd.ctab = (uint32_t)&gCpo;
	gSbd.bits = PVMEM_FB_BASE;
	gSbd.pixelSize = 0;

	/* step 2: hardware depth for the DAL's D */
	PV_DISP_DEPTH = (uint32_t)depth_code(gD);
	PV_DISP_STRIDE = gW * gD / 8;

	/* step 3: palette */
	if (pal) {
		uint32_t n = tab_count(pal);

		hal_display_set_palette(0, n, pal + 4);		/* positional; nothing when n > 256 */
		copy_bytes((uint32_t)&gCpo, (uint32_t)pal, 4 + 4 * (n > 256 ? 256 : n));
		if (n == 256 && !gStdValid) {			/* [X] §5: first 256-entry table = OS standard table */
			copy_bytes((uint32_t)gStd, (uint32_t)(pal + 4), sizeof(gStd));
			gStdValid = 1;
		}
	}

	/* step 4 */
	hal_screen_update_bitmap(gD);
	return 0;
}

uint32_t hal_screen_get_colortable(void)
{
	return (uint32_t)&gCpo;
}

/* ---------------------------------------------------------------- colour lookup ([S] §5..§6, [F], [X] §5..§6) */
static int matches_std(const uint8_t *ref)	/* entries 0..229 and 255 have the standard table's R,G,B */
{
	const uint8_t *e = ref + 4;
	uint32_t i;

	if (!gStdValid || tab_count(ref) != 256)
		return 0;
	for (i = 0; i < 256; i++) {
		if (i >= 230 && i < 255)
			continue;
		if (e[4 * i + 1] != gStd[i][1] || e[4 * i + 2] != gStd[i][2] || e[4 * i + 3] != gStd[i][3])
			return 0;
	}
	return 1;
}

static void recheck(const uint8_t *ref)
{
	if (gPalState != PAL_UNCHECKED || !gStdValid || tab_count(ref) != 256)
		return;
	gPalState = matches_std(ref) ? PAL_STANDARD : PAL_NONSTANDARD;
}

static int is_grey(const uint8_t *e, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++, e += 4)
		if (e[1] != e[2] || e[2] != e[3])
			return 0;
	return 1;
}

static int same4(const uint8_t *a, const uint8_t *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int same_rgb(const uint8_t *a, const uint8_t *b)
{
	return a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

#if DAL_COLOR_MODE == DAL_COLOR_REFERENCE
/* 4R: colour-cube answer for the standard 256-entry palette ([X] §6) */
static const struct { uint8_t r, g, b, idx; } gCubeExc[24] = {
	{ 0, 0, 0, 255 }, { 0, 1, 1, 215 }, { 0, 3, 0, 228 }, { 0, 3, 1, 228 },
	{ 0, 3, 3, 229 }, { 1, 0, 1, 215 }, { 1, 1, 0, 215 }, { 1, 1, 1, 216 },
	{ 1, 3, 0, 228 }, { 1, 3, 1, 228 }, { 1, 3, 3, 229 }, { 2, 2, 2, 218 },
	{ 3, 0, 0, 226 }, { 3, 0, 1, 226 }, { 3, 0, 3, 227 }, { 3, 1, 0, 226 },
	{ 3, 1, 1, 226 }, { 3, 1, 3, 227 }, { 3, 3, 3, 220 }, { 4, 4, 4, 222 },
	{ 4, 4, 5, 225 }, { 4, 5, 4, 225 }, { 5, 4, 4, 225 }, { 5, 5, 5, 224 },
};

static uint32_t cube_index(const uint8_t *e)
{
	const uint32_t kr = (e[1] + 25u) / 51, kg = (e[2] + 25u) / 51, kb = (e[3] + 25u) / 51;
	uint32_t i, half, bsub;

	for (i = 0; i < sizeof(gCubeExc) / sizeof(gCubeExc[0]); i++)
		if (gCubeExc[i].r == kr && gCubeExc[i].g == kg && gCubeExc[i].b == kb)
			return gCubeExc[i].idx;
	/* Palm OS standard palette 0..215: halves by blue (FF,CC,99 | 66,33,00), red every 18, blue every 6, green every 1 */
	half = kb >= 3 ? 0 : 1;
	bsub = half ? 2 - kb : 5 - kb;
	return half * 108 + (5 - kr) * 18 + bsub * 6 + (5 - kg);
}
#else
static uint32_t true_dist(const uint8_t *a, const uint8_t *b)
{
	const int32_t dr = (int32_t)a[1] - b[1], dg = (int32_t)a[2] - b[2], db = (int32_t)a[3] - b[3];

	return (uint32_t)(dr * dr + dg * dg + db * db);
}
#endif

static uint32_t intensity(const uint8_t *e) { return (2u * e[1] + 5u * e[2] + e[3]) / 8; }

static uint32_t nearest(const uint8_t *rec, const uint8_t *ref)
{
	const uint32_t n = tab_count(ref);
	const uint8_t *e = ref + 4;
	uint32_t i, best, bestd;
	int grey;

	if (n == 0)								/* 1 */
		return rec[0];
	if (rec[0] < n && same4(e + 4 * rec[0], rec))		/* 2 */
		return rec[0];
	if (same_rgb(e + 4 * (n - 1), rec))					/* 3 */
		return n - 1;

	grey = is_grey(e, n);
#if DAL_COLOR_MODE == DAL_COLOR_REFERENCE
	if (gPalState == PAL_STANDARD && n == 256)			/* 4R */
		return cube_index(rec);
#else
	if (!grey) {										/* 4N */
		const uint8_t *t = (n == 256 && matches_std(ref)) ? &gStd[0][0] : e;

		best = n - 1;
		bestd = true_dist(t + 4 * (n - 1), rec);
		for (i = 0; i + 1 < n; i++) {
			const uint32_t d = true_dist(t + 4 * i, rec);

			if (d < bestd) {
				bestd = d;
				best = i;
			}
		}
		return best;
	}
#endif

	for (i = 0; i < n; i++)								/* 5 */
		if (same4(e + 4 * i, rec))
			return i;

	best = 0;											/* 6 */
	bestd = 0xffffffffu;
	for (i = 0; i < n; i++) {
		const uint8_t *c = e + 4 * i;
		uint32_t d;

		if (grey) {
			const int32_t di = (int32_t)intensity(c) - (int32_t)intensity(rec);

			d = (uint32_t)(di < 0 ? -di : di);
		} else {
			const int32_t dr = (int8_t)(uint8_t)(c[1] - rec[1]);
			const int32_t dg = (int8_t)(uint8_t)(c[2] - rec[2]);
			const int32_t db = (int8_t)(uint8_t)(c[3] - rec[3]);

			d = (uint32_t)(dr * dr + dg * dg + db * db);
		}
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

uint32_t hal_draw_find_indexes(uint32_t count, uint8_t *recs, const void *tableP)
{
	const uint8_t *ref = tableP ? (const uint8_t *)tableP : (const uint8_t *)&gCpo;
	uint32_t i;

	recheck(ref);
	if (recs)
		for (i = 0; i < count; i++, recs += 4)
			recs[0] = (uint8_t)nearest(recs, ref);
	return 0;
}

uint32_t hal_screen_palette(int32_t start, uint32_t count, uint32_t depth, const void *tableP, const void *const *palettes)
{
	static uint8_t *const xlat[4] = { gXlat1, gXlat2, gXlat4, gXlat8 };
	static const uint16_t cap[4] = { 2, 4, 16, 256 };
	const uint8_t *ref = tableP ? (const uint8_t *)tableP : (const uint8_t *)&gCpo;
	uint32_t k, j;

	gPalState = PAL_UNCHECKED;							/* 1 */
	if (depth <= 8)										/* 2 */
		recheck(ref);
	if (palettes && palettes[4])						/* 3 */
		hal_display_set_palette(start, count, (const uint8_t *)palettes[4] + 4);
	for (k = 0; palettes && k < 4; k++) {				/* 4 */
		const uint8_t *src = palettes[k];
		uint32_t n;

		if (!src)
			continue;
		n = tab_count(src);
		if (n > cap[k])
			n = cap[k];
		for (j = 0; j < n; j++)
			xlat[k][j] = depth > 8 ? 0 : (uint8_t)nearest(src + 4 + 4 * j, ref);
	}
	return 0;
}

uint32_t hal_screen_default_palette(void)
{
	return gPalState == PAL_STANDARD;
}

/* ---------------------------------------------------------------- screen lock ([S] §7, [X] §7) */
static int lock_take(void)		/* returns 1 when the mutex was taken (scheduler running) */
{
	if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		if (!gLockMtx)
			gLockMtx = xSemaphoreCreateRecursiveMutex();
		return 0;
	}
	if (!gLockMtx) {
		vTaskSuspendAll();
		if (!gLockMtx)
			gLockMtx = xSemaphoreCreateRecursiveMutex();
		(void)xTaskResumeAll();
	}
	if (!gLockMtx)
		return 0;
	xSemaphoreTakeRecursive(gLockMtx, portMAX_DELAY);
	return 1;
}

static void lock_give(int taken)
{
	if (taken)
		xSemaphoreGiveRecursive(gLockMtx);
}

uint32_t hal_screen_lock(uint32_t mode, int *fatal)
{
	const uint32_t copy = PVMEM_FB_BASE + ((gW * gH * 2 + 0xfffu) & ~0xfffu);
	const uint32_t size = gW * gH * gD / 8;
	const int taken = lock_take();

	if (gLockCount == 255 ||
	    (gLockCount == 0 && (mode > 2 || copy + size > PVMEM_FB_BASE + VRAM_SIZE))) {
		*fatal = 1;
		lock_give(taken);
		return PVMEM_FB_BASE;
	}
	*fatal = 0;
	if (++gLockCount == 1) {
		copy_bytes(copy, PVMEM_FB_BASE, size);
		PV_DISP_BASE = copy;
		if (mode == 1)
			fill_bytes(PVMEM_FB_BASE, gSbd.pixelSize == 16 ? 0xFF : 0x00, size);
	}
	lock_give(taken);
	return PVMEM_FB_BASE;
}

uint32_t hal_screen_unlock(void)
{
	const int taken = lock_take();

	if (gLockCount > 0 && --gLockCount == 0)
		PV_DISP_BASE = PVMEM_FB_BASE;
	lock_give(taken);
	return 0;
}
