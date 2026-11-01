/* pv-vz90-dal g4s: HALDisplayDrawBootScreen (slot 0x84) - draws the OS boot image straight into the framebuffer.
 * Requirements: local functional spec HALDisplayDrawBootScreen (analyst role) and the public Palm OS BitmapType
 * layouts V0..V3 (BitmapTypeV2/V3, BitmapFlagsType, BitmapDirectInfoType, ColorTableType). A version byte with
 * bit 0x80 set marks a little-endian (ARM-native) bitmap: its fields are little-endian and its flag bits are
 * numbered from bit 0 (compressed) upward, as in the DAL-side descriptors (observed on this ROM).
 * Compressed data is expanded with bmp_codec.c. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "hal_bootscreen.h"
#include "hal_display.h"
#include "hal_draw.h"
#include "bmp_codec.h"
#include "hal_memory.h"

void *pvPortMalloc(unsigned int size);
void vPortFree(void *p);
void dbg_puts(const char *s);
void dbg_hex(uint32_t v);

enum { F_COMPRESSED = 1, F_COLORTABLE = 2, F_TRANSPARENT = 4, F_INDIRECT = 8, F_DIRECT = 0x20 };

struct member {
	int32_t w, h;
	uint32_t rowBytes, flags, depth, version, le, fmt, compression, transparent, next, ctabCount, encLen;
	const uint8_t *ctab, *bits, *base;
};

static uint32_t rd(const uint8_t *p, uint32_t n, uint32_t le)
{
	uint32_t v = 0, i;

	for (i = 0; i < n; i++)
		v |= (uint32_t)p[le ? i : n - 1 - i] << (le ? 8 * i : 8 * (n - 1 - i));
	return v;
}

/* the whole object [p, p + len) must lie in ROM or in RAM (the PV memory map, hal_memory.h) */
static int in_memory(const uint8_t *p, uint32_t len)
{
	uint32_t a = (uint32_t)p;

	if (a >= PVMEM_ROM_BASE && a < PVMEM_ROM_BASE + PVMEM_ROM_SIZE)
		return len <= PVMEM_ROM_BASE + PVMEM_ROM_SIZE - a;
	if (a >= 0x20000000u && a < 0x20000000u + PVMEM_RAM_REQUIRED)
		return len <= 0x20000000u + PVMEM_RAM_REQUIRED - a;
	return 0;
}

/* big-endian flags have compressed in bit 15 down to noDither in bit 8; bring them to the bit-0-first order */
static uint32_t flags_lsb_first(uint32_t be)
{
	uint32_t out = 0, i;

	for (i = 0; i < 8; i++)
		if (be & (0x8000u >> i))
			out |= 1u << i;
	return out;
}

#define MAX_SIDE	2048u		/* larger than any panel or boot image; rejects garbage headers */

/* Reads one family member. Returns 1 for a usable member, 0 for an invalid or unreadable one (nothing drawn).
 * Every field read is checked to lie in memory, every size against the others before use. A member whose pixel
 * size is 0xFF (the high-density section marker) is reported with depth 0xFF and no pixel data. */
static int parse(const uint8_t *p, struct member *m)
{
	uint32_t hdr, minRow;
	const uint8_t *q;

	if (!in_memory(p, 16))
		return 0;
	m->base = p;
	m->version = p[9] & 0x7f;
	m->le = (p[9] & 0x80) != 0;
	m->w = (int16_t)rd(p, 2, m->le);
	m->h = (int16_t)rd(p + 2, 2, m->le);
	m->rowBytes = rd(p + 4, 2, m->le);
	m->flags = m->le ? rd(p + 6, 2, 1) : flags_lsb_first(rd(p + 6, 2, 0));
	m->depth = m->version == 0 ? 1 : p[8];
	m->fmt = 0;
	m->transparent = 0;
	m->compression = BMP_SCANLINE;
	m->ctab = 0;
	m->ctabCount = 0;
	m->bits = 0;
	m->encLen = 0;
	if (m->version > 3)
		return 0;
	if (m->version == 3) {
		if (!in_memory(p, 24) || p[10] < 24 || p[10] > 64)
			return 0;
		hdr = p[10];
		m->fmt = p[11];
		m->compression = p[13];
		m->transparent = rd(p + 16, 4, m->le);
		m->next = rd(p + 20, 4, m->le);
	} else {
		hdr = 16;
		m->next = m->version >= 1 ? 4 * rd(p + 10, 2, m->le) : 0;
		if (m->version == 2) {
			m->transparent = p[12];
			m->compression = p[13];
		}
	}
	if (m->next && (m->next < hdr || m->next > 0x00400000u || !in_memory(p, m->next + 16)))
		m->next = 0;				/* an implausible link ends the chain */
	if (m->depth == 0xff)
		return 1;				/* density section marker: no pixels */
	if (m->depth != 1 && m->depth != 2 && m->depth != 4 && m->depth != 8 && m->depth != 16)
		return 0;
	minRow = ((uint32_t)m->w * m->depth + 7) / 8;
	if (m->w <= 0 || m->h <= 0 || (uint32_t)m->w > MAX_SIDE || (uint32_t)m->h > MAX_SIDE || m->rowBytes < minRow ||
	    m->rowBytes > MAX_SIDE * 2)
		return 0;
	q = p + hdr;
	if (m->flags & F_COLORTABLE) {		/* ColorTableType: u16 entry count, then RGBColorType entries */
		if (!in_memory(q, 2))
			return 0;
		m->ctabCount = rd(q, 2, m->le);
		if (m->ctabCount > 256 || !in_memory(q, 2 + 4 * m->ctabCount))
			return 0;
		m->ctab = q + 2;
		q += 2 + 4 * m->ctabCount;
	}
	if (m->version == 2 && m->depth == 16 && (m->flags & F_DIRECT)) {	/* BitmapDirectInfoType */
		if (!in_memory(q, 8))
			return 0;
		m->transparent = ((uint32_t)(q[5] & 0xf8) << 8) | ((uint32_t)(q[6] & 0xfc) << 3) | (q[7] >> 3);
		q += 8;
	}
	if (m->flags & F_INDIRECT) {
		if (!in_memory(q, 4))
			return 0;
		q = (const uint8_t *)rd(q, 4, 1);
	}
	if (m->flags & F_COMPRESSED) {		/* size prefix (16-bit up to V2, 32-bit in V3), counting itself */
		uint32_t pw = m->version >= 3 ? 4 : 2, size;

		if (!in_memory(q, pw))
			return 0;
		size = rd(q, pw, m->le);
		if (size < pw || !in_memory(q, size))
			return 0;
		m->encLen = size - pw;
		q += pw;
	} else if (!in_memory(q, m->rowBytes * (uint32_t)m->h))
		return 0;
	m->bits = q;
	return 1;
}

static uint32_t src_px(const struct member *m, const uint8_t *bits, int32_t x, int32_t y)
{
	const uint8_t *row = bits + (uint32_t)y * m->rowBytes;
	uint32_t d = m->depth, per, k;

	if (d == 16)
		return (m->le || m->fmt == 2) ? (uint32_t)(row[2 * x] | (row[2 * x + 1] << 8)) :
						 (uint32_t)((row[2 * x] << 8) | row[2 * x + 1]);
	if (d == 8)
		return row[x];
	per = 8 / d;
	k = (uint32_t)x % per;
	if (m->fmt == 3)
		return (row[(uint32_t)x / per] >> (k * d)) & ((1u << d) - 1);
	return (row[(uint32_t)x / per] >> (8 - d * (k + 1))) & ((1u << d) - 1);
}

uint32_t hal_display_draw_boot_screen(int32_t x0, int32_t y0, const uint8_t *bmp)
{
	struct member m, n;
	uint32_t W, H, D, num = 1, den = 1, k, lut[256];
	int32_t x, y, ew, eh;
	uint8_t *plain = 0;
	const uint8_t *bits;
	volatile uint16_t *fb = (volatile uint16_t *)PVMEM_FB_BASE;

	hal_display_geometry(&W, &H, &D);
	if (!bmp || !W || !parse(bmp, &m))
		return 0;
	/* member choice: advance while this member and the next one are both shallower than the screen (spec) */
	if (m.depth == 0xff)
		return 0;
	while (m.depth < D && m.next) {
		if (!parse(m.base + m.next, &n) || n.depth == 0xff || n.depth >= D)
			break;
		m = n;
	}
	if (D != 16 || (m.depth != 1 && m.depth != 2 && m.depth != 4 && m.depth != 8 && m.depth != 16)) {
		dbg_puts("g4s: HALDisplayDrawBootScreen: depth ");
		dbg_hex(m.depth);
		dbg_puts(" onto ");
		dbg_hex(D);
		dbg_puts(" bpp not drawn\n");
		return 0;
	}
	/* scale by size (each test matches on either dimension) */
	{
		static const uint8_t up[5][2] = { { 1, 1 }, { 3, 2 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };
		static const uint8_t down[2][2] = { { 2, 3 }, { 1, 2 } };
		int found = 0;

		for (k = 0; k < 5 && !found; k++)
			if ((uint32_t)m.w * up[k][0] / up[k][1] == W || (uint32_t)m.h * up[k][0] / up[k][1] == H) {
				num = up[k][0];
				den = up[k][1];
				found = 1;
			}
		for (k = 0; k < 2 && !found; k++)
			if ((uint32_t)m.w * down[k][0] / down[k][1] == W || (uint32_t)m.h * down[k][0] / down[k][1] == H) {
				num = down[k][0];
				den = down[k][1];
				found = 1;
			}
	}				/* 1/3 and 1/4 sizes stay 1:1, as in the reference */
	if (m.flags & F_COMPRESSED) {
		plain = (uint8_t *)pvPortMalloc(m.rowBytes * (uint32_t)m.h);
		if (!plain || bmp_unpack(m.version < 2 ? BMP_SCANLINE : m.compression, m.bits, m.encLen, plain, m.rowBytes,
					 (uint32_t)m.h, m.depth, 0)) {
			dbg_puts("g4s: HALDisplayDrawBootScreen: compressed image not drawn\n");
			if (plain)
				vPortFree(plain);
			return 0;
		}
		bits = plain;
	} else
		bits = m.bits;
	if (m.depth <= 8) {
		uint8_t rec[4];

		for (k = 0; k < (1u << m.depth); k++) {
			if (m.ctab && k < m.ctabCount)	/* entries the table lacks fall back to the standard palette */
				rec[1] = m.ctab[4 * k + 1], rec[2] = m.ctab[4 * k + 2], rec[3] = m.ctab[4 * k + 3];
			else
				hal_draw_std_colour(m.depth, k, rec);
			lut[k] = ((uint32_t)(rec[1] & 0xf8) << 8) | ((uint32_t)(rec[2] & 0xfc) << 3) | (rec[3] >> 3);
		}
	}
	ew = (int32_t)((uint32_t)m.w * num / den);	/* scaled image extent, placed at (x0, y0) and clipped */
	eh = (int32_t)((uint32_t)m.h * num / den);
	for (y = y0 < 0 ? 0 : y0; y < y0 + eh && y < (int32_t)H; y++) {
		int32_t sy = (int32_t)((uint32_t)(y - y0) * den / num);

		for (x = x0 < 0 ? 0 : x0; x < x0 + ew && x < (int32_t)W; x++) {
			int32_t sx = (int32_t)((uint32_t)(x - x0) * den / num);
			uint32_t v = src_px(&m, bits, sx, sy);

			if ((m.flags & F_TRANSPARENT) && v == (m.depth == 16 ? (m.transparent & 0xffff) : m.transparent))
				continue;
			fb[(uint32_t)y * W + (uint32_t)x] = (uint16_t)(m.depth == 16 ? v : lut[v]);
		}
	}
	if (plain)
		vPortFree(plain);
	return 0;
}
