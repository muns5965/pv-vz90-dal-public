/* pv-vz90-dal g4u: drawing self-test for indexed (1/2/4/8 bpp) destinations, run on the target when the image is
 * built with DAL_SELFTEST=1 (local test images only; normal images do not call it). Every case draws into small
 * off-screen bitmaps through the same entry points the OS uses (hal_draw_*) and checks each pixel, including the
 * padding bytes past the row (stride) and the pixels outside the clip rectangle. Expected values are written out
 * by hand from the public Palm OS semantics of the transfer modes. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "hal_draw.h"
#include "selftest_draw.h"
#include "hal_display.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);

enum { W = 12, H = 6, RB8 = 16, SENT = 0x77 };

static uint8_t gBits[RB8 * H], gSrcBits[RB8 * H], gCtab[4 + 256 * 4];
static uint8_t gDst[0x24], gSrc[0x24], gState[0x30], gCanvas[0x10];
static uint32_t gFail, gChecks;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void desc(uint8_t *d, uint32_t w, uint32_t h, uint32_t rb, uint32_t depth, uint32_t fmt, uint32_t flags,
		 const uint8_t *ctab, uint8_t *bits, uint32_t tv)
{
	uint32_t i;

	for (i = 0; i < 0x24; i++)
		d[i] = 0;
	put16(d + 0, w);
	put16(d + 2, h);
	put16(d + 4, rb);
	put16(d + 6, flags);
	d[8] = (uint8_t)depth;
	d[9] = 3;
	d[0x0a] = 0x28;
	d[0x0b] = (uint8_t)fmt;
	d[0x0d] = 0xff;
	put16(d + 0x0e, 72);
	put32(d + 0x10, tv);
	put32(d + 0x18, (uint32_t)ctab);
	put32(d + 0x1c, (uint32_t)bits);
}

static void state(uint32_t mode, uint32_t pat, uint32_t fg, uint32_t bg, uint32_t tx)
{
	uint32_t i;

	for (i = 0; i < sizeof(gState); i++)
		gState[i] = 0;
	gState[0] = (uint8_t)mode;
	gState[1] = (uint8_t)pat;
	gState[0x10] = (uint8_t)fg;
	gState[0x11] = (uint8_t)bg;
	gState[0x12] = (uint8_t)tx;
}

static void canvas(int32_t l, int32_t t, int32_t w, int32_t h)
{
	put16(gCanvas + 0, (uint32_t)l);
	put16(gCanvas + 2, (uint32_t)t);
	put16(gCanvas + 4, (uint32_t)w);
	put16(gCanvas + 6, (uint32_t)h);
	put32(gCanvas + 8, (uint32_t)gState);
	put32(gCanvas + 0x0c, (uint32_t)gDst);
}

static void fill_bits(uint8_t *b, uint32_t v)
{
	uint32_t i;

	for (i = 0; i < RB8 * H; i++)
		b[i] = (uint8_t)v;
}

static void expect(const char *what, uint32_t got, uint32_t want, uint32_t x, uint32_t y)
{
	gChecks++;
	if (got == want)
		return;
	if (gFail++ < 12) {
		dbg_puts("g4u: SELFTEST FAIL ");
		dbg_puts(what);
		dbg_puts(" at ");
		dbg_hex((x << 16) | y);
		dbg_puts(" got ");
		dbg_hex(got);
		dbg_puts(" want ");
		dbg_hex(want);
		dbg_puts("\n");
	}
}

static uint32_t px(uint32_t depth, uint32_t fmt, uint32_t x, uint32_t y)	/* reads gBits independently */
{
	uint32_t per = 8 / depth, byte = gBits[y * RB8 + x / per], k = x % per;

	if (depth == 8)
		return gBits[y * RB8 + x];
	return fmt == 3 ? (byte >> (k * depth)) & ((1u << depth) - 1) : (byte >> (8 - depth * (k + 1))) & ((1u << depth) - 1);
}

static void check_padding(const char *what)
{
	uint32_t y, x;

	for (y = 0; y < H; y++)
		for (x = W; x < RB8; x++)
			expect(what, gBits[y * RB8 + x], SENT, x, y);
}

void selftest_draw(void)
{
	static const int16_t rect[4] = { 2, 1, 6, 3 };		/* x 2..7, y 1..3 */
	uint32_t x, y, i;
	uint8_t rec[4];

	gFail = gChecks = 0;
	put16(gCtab, 256);					/* a standard 256-entry colour table for the destinations */
	for (i = 0; i < 256; i++) {
		hal_draw_std_colour(8, i, rec);
		gCtab[4 + 4 * i] = (uint8_t)i;
		gCtab[5 + 4 * i] = rec[1];
		gCtab[6 + 4 * i] = rec[2];
		gCtab[7 + 4 * i] = rec[3];
	}

	/* 1. 8 bpp filled rectangle, winPaint, clip cuts the right part (x >= 6) */
	fill_bits(gBits, SENT);
	desc(gDst, W, H, RB8, 8, 0, 0, gCtab, gBits, 0);
	state(0, 0, 0x23, 0x45, 0);
	canvas(0, 0, 6, H);
	hal_draw_rectangle(gCanvas, rect, 0, 0);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			expect("rect8", px(8, 0, x, y), (x >= 2 && x < 6 && y >= 1 && y < 4) ? 0x23 : SENT, x, y);
	check_padding("rect8 stride");

	/* 2. gray pattern (AA/55 rows, one bit per pixel): bit 1 -> fore, bit 0 -> back */
	fill_bits(gBits, SENT);
	state(0, 2, 0x10, 0x20, 0);
	canvas(0, 0, W, H);
	hal_draw_rectangle(gCanvas, rect, 0, 0);
	for (y = 1; y < 4; y++)
		for (x = 2; x < 8; x++)
			expect("gray8", px(8, 0, x, y), ((((y & 1) ? 0x55u : 0xaau) >> (7 - (x & 7))) & 1) ? 0x10 : 0x20, x, y);

	/* 3. winInvert on 8 bpp: index XOR NOT fore, kept to 8 bits */
	fill_bits(gBits, 0x0f);
	state(3, 0, 0xf0, 0, 0);
	hal_draw_rectangle(gCanvas, rect, 0, 0);
	expect("invert8", px(8, 0, 2, 1), (0x0f ^ ~0xf0u) & 0xff, 2, 1);
	expect("invert8 outside", px(8, 0, 1, 1), 0x0f, 1, 1);

	/* 4. diagonal line (0,0)-(3,3) width 1 */
	fill_bits(gBits, 0);
	state(0, 0, 9, 0, 0);
	hal_draw_line(gCanvas, 0, 0, 3, 3, 1);
	for (i = 0; i < 4; i++)
		expect("line8", px(8, 0, i, i), 9, i, i);
	expect("line8 off", px(8, 0, 1, 0), 0, 1, 0);

	/* 5. 4 bpp destinations, both pixel orders: neighbours of the written nibble stay unchanged */
	for (i = 0; i < 2; i++) {
		uint32_t fmt = i ? 3 : 0;

		fill_bits(gBits, 0x00);
		desc(gDst, W, H, RB8, 4, fmt, 0, gCtab, gBits, 0);
		state(0, 0, 0xd, 0, 0);
		{
			static const int16_t one[4] = { 3, 2, 1, 1 };

			hal_draw_rectangle(gCanvas, one, 0, 0);
		}
		for (x = 0; x < 8; x++)
			expect(fmt ? "rect4 LE" : "rect4", px(4, fmt, x, 2), x == 3 ? 0xd : 0, x, 2);
	}

	/* 6. 1 bpp destination */
	fill_bits(gBits, 0x00);
	desc(gDst, W, H, RB8, 1, 0, 0, 0, gBits, 0);
	state(0, 0, 1, 0, 0);
	hal_draw_rectangle(gCanvas, rect, 0, 0);
	for (x = 0; x < W; x++)
		expect("rect1", px(1, 0, x, 2), x >= 2 && x < 8, x, 2);

	/* 7. 8 bpp source -> 8 bpp destination, same depth: indexes copied; with transparency only others drawn */
	fill_bits(gBits, SENT);
	desc(gDst, W, H, RB8, 8, 0, 0, gCtab, gBits, 0);
	for (i = 0; i < RB8 * H; i++)
		gSrcBits[i] = (uint8_t)(i & 7);
	desc(gSrc, W, H, RB8, 8, 0, 0, 0, gSrcBits, 0);
	state(0, 0, 0, 0, 0);
	{
		static const int16_t r2[4] = { 0, 0, W, H };

		hal_draw_bitmap(gCanvas, gSrc, r2, 0, 0);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				expect("blit8", px(8, 0, x, y), (y * RB8 + x) & 7, x, y);
		check_padding("blit8 stride");
		fill_bits(gBits, SENT);
		desc(gSrc, W, H, RB8, 8, 0, 4, 0, gSrcBits, 3);	/* hasTransparency, transparent index 3 */
		hal_draw_bitmap(gCanvas, gSrc, r2, 0, 0);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				expect("blit8 transparent", px(8, 0, x, y), ((y * RB8 + x) & 7) == 3 ? SENT : (y * RB8 + x) & 7, x, y);
	}

	/* 8. 1 bpp source without colour table -> 8 bpp: bit 1 -> fore index, bit 0 -> back index */
	fill_bits(gBits, SENT);
	for (i = 0; i < RB8 * H; i++)
		gSrcBits[i] = 0xa5;
	desc(gSrc, W, H, 2, 1, 0, 0, 0, gSrcBits, 0);
	state(0, 0, 0x31, 0x32, 0);
	{
		static const int16_t r3[4] = { 0, 0, 8, 1 };

		hal_draw_bitmap(gCanvas, gSrc, r3, 0, 0);
		for (x = 0; x < 8; x++)
			expect("1to8", px(8, 0, x, 0), ((0xa5 >> (7 - x)) & 1) ? 0x31 : 0x32, x, 0);
	}

	/* 9. 16 bpp source -> 8 bpp: each pixel becomes the index the colour lookup gives for its colour */
	fill_bits(gBits, SENT);
	{
		static const uint16_t c16[4] = { 0xf800, 0x07e0, 0x001f, 0xffff };	/* red, green, blue, white (LE 565) */
		static const int16_t r4[4] = { 0, 0, 4, 1 };

		for (i = 0; i < 4; i++)
			put16(gSrcBits + 2 * i, c16[i]);
		desc(gSrc, 4, 1, 8, 16, 2, 0, 0, gSrcBits, 0);
		state(0, 0, 0, 0, 0);
		hal_draw_bitmap(gCanvas, gSrc, r4, 0, 0);
		for (i = 0; i < 4; i++) {
			uint32_t v = c16[i];

			rec[0] = 0;
			rec[1] = (uint8_t)((v >> 11) * 8 + ((v >> 11) & 7));
			rec[2] = (uint8_t)(((v >> 5) & 0x3f) * 4 + (((v >> 5) & 0x3f) & 3));
			rec[3] = (uint8_t)((v & 0x1f) * 8 + ((v & 0x1f) & 7));
			hal_draw_find_indexes(1, rec, gCtab);
			expect("16to8", px(8, 0, i, 0), rec[0], i, 0);
		}
	}

	/* 10. GetPixel on an indexed bitmap returns the index */
	expect("getpixel8", hal_draw_get_pixel(gDst, 1, 0, 1), px(8, 0, 1, 0), 1, 0);

	dbg_puts(gFail ? "g4u: SELFTEST indexed drawing FAILED, checks " : "g4u: SELFTEST indexed drawing passed, checks ");
	dbg_hex(gChecks);
	dbg_puts(" failures ");
	dbg_hex(gFail);
	dbg_puts("\n");
}

#if DAL_LOG
/* g4w: positive test of the stop diagnostics (src/probe.c), logging test images only. A fake register/stack
 * frame holds three pointers to valid NUL-terminated text (ROM, RAM, stack) and four that must be rejected
 * (80 bytes without a NUL, an unmapped address, an address at the top of the address space, a short string).
 * Expected output between the two marker lines: exactly the three "selftest-..." texts. */
void probe_stop_context(const uint32_t *args);

static char gTxtRam[16] = "selftest-ram";
static char gTxtStack[16] = "selftest-stack";
static char gNoNul[80];

void selftest_stop_context(void)
{
	static const char rom[] = "selftest-rom";
	static uint32_t frame[6 + 48];
	uint32_t i;

	for (i = 0; i < sizeof(gNoNul); i++)
		gNoNul[i] = 'A';
	frame[1] = (uint32_t)rom;			/* r0: text in the DAL image (ROM) */
	frame[2] = (uint32_t)gTxtRam;			/* r1: text in RAM */
	frame[3] = (uint32_t)gNoNul;			/* r2: no NUL within 72 bytes -> rejected */
	frame[4] = 0x40000000u;				/* r3: unmapped -> rejected */
	frame[6] = (uint32_t)gTxtStack;			/* [sp+0]: text in RAM */
	frame[7] = 0xfffffff0u;				/* [sp+4]: top of the address space -> rejected */
	frame[8] = (uint32_t)"ab";			/* [sp+8]: shorter than 3 characters -> rejected */
	dbg_puts("g4w: SELFTEST stop diagnostics begin (expect selftest-rom, selftest-ram, selftest-stack)\n");
	probe_stop_context(frame);
	dbg_puts("g4w: SELFTEST stop diagnostics end\n");
}
#endif
