/* pv-vz90-dal g4r/g4s: HALDraw rendering (slots 0xBE Bitmap, 0xC0 GetPixel, 0xC1 Chars, 0xC2 Line, 0xC3 Rectangle,
 * 0xC4 Pixels) for 16 bpp and indexed (1/2/4/8 bpp) destinations. Requirements: local functional specs (analyst
 * role) HALDraw, HALDrawIface and HALDrawCompressed, public Palm OS data layouts (BitmapTypeV3, FontType,
 * RectangleType, PointType, RGBColorType, WinDrawOperation, BitmapCompressionType); see docs/INTERFACE.md §32-33.
 * Compressed data is encoded/expanded by bmp_codec.c.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "hal_draw.h"
#include "bmp_codec.h"
#include "hal_display.h"

void *pvPortMalloc(unsigned int size);
void vPortFree(void *p);

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
#define DRAW_DEBUG 0	/* 1: dump the draw state of the first draws (local diagnosis) */
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);

enum { M_PAINT, M_ERASE, M_MASK, M_INVERT, M_OVERLAY, M_PAINT_INV, M_SWAP };

/* ---- descriptors ---- */
struct surf {
	uint8_t *bits;
	int32_t w, h, rowBytes;
	uint32_t depth, fmt, density, flags;
	const uint8_t *ctab;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void surf_of(const uint8_t *d, struct surf *s)
{
	s->w = (int16_t)rd16(d + 0);
	s->h = (int16_t)rd16(d + 2);
	s->rowBytes = rd16(d + 4);
	s->flags = rd16(d + 6);
	s->depth = d[8];
	s->fmt = d[0x0b];
	s->density = rd16(d + 0x0e);
	s->ctab = (const uint8_t *)rd32(d + 0x18);
	s->bits = (uint8_t *)rd32(d + 0x1c);
	if (!s->density)
		s->density = 72;
}

static uint32_t swap16(uint32_t v) { return ((v & 0xff) << 8) | ((v >> 8) & 0xff); }

static uint32_t rgb565(const uint8_t *c)		/* RGBColorType {index, r, g, b}: truncation */
{
	return ((uint32_t)(c[1] & 0xf8) << 8) | ((uint32_t)(c[2] & 0xfc) << 3) | (c[3] >> 3);
}

/* Palm OS standard 8 bpp palette: a 6x6x6 colour cube in two halves of blue levels (red every 18, blue every 6;
 * checked against reference screenshots), then ten greys (descending),
 * five extra colours and black; component level k is 0xFF - 0x33*k (derived from the index facts in the spec) */
static void std_rgb(uint32_t i, uint8_t *c)
{
	static const uint8_t grey[10] = { 0x11, 0x22, 0x44, 0x55, 0x77, 0x88, 0xaa, 0xbb, 0xdd, 0xee };
	static const uint8_t extra[5][3] = { { 0xc0, 0xc0, 0xc0 }, { 0x80, 0, 0 }, { 0x80, 0, 0x80 }, { 0, 0x80, 0 }, { 0, 0x80, 0x80 } };

	c[0] = (uint8_t)i;
	if (i < 215) {
		uint32_t half = i / 108, rest = i % 108, r = rest / 18, b = half * 3 + (rest % 18) / 6, g = rest % 6;

		c[1] = (uint8_t)(0xff - 0x33 * r);
		c[2] = (uint8_t)(0xff - 0x33 * g);
		c[3] = (uint8_t)(0xff - 0x33 * b);
	} else if (i < 225) {
		c[1] = c[2] = c[3] = grey[i - 215];
	} else if (i < 230) {
		c[1] = extra[i - 225][0];
		c[2] = extra[i - 225][1];
		c[3] = extra[i - 225][2];
	} else
		c[1] = c[2] = c[3] = 0;
}

static uint32_t std_index(uint32_t depth, uint32_t i)	/* source index without colour table -> standard 8 bpp index */
{
	static const uint8_t d2[4] = { 0x00, 0xdd, 0xda, 0xff };
	static const uint8_t d4[16] = { 0x00, 0xe0, 0xdf, 0x19, 0xde, 0xdd, 0x32, 0xdc, 0xdb, 0xa5, 0xda, 0xd9, 0xbe, 0xd8, 0xd7, 0xff };

	return depth == 2 ? d2[i & 3] : depth == 4 ? d4[i & 15] : i & 0xff;
}

void hal_draw_std_colour(uint32_t depth, uint32_t index, uint8_t *rec)	/* standard palette of a depth (g4s) */
{
	if (depth == 1) {				/* 1 bpp: 0 white, 1 black */
		rec[0] = (uint8_t)index;
		rec[1] = rec[2] = rec[3] = index & 1 ? 0 : 0xff;
		return;
	}
	std_rgb(std_index(depth, index), rec);
}

static inline __attribute__((always_inline)) uint32_t dmask(const struct surf *d)
{
	return d->depth >= 16 ? 0xffffu : (1u << d->depth) - 1;
}

/* stored pixel value: 16 bpp halfword as stored, or the index of an indexed bitmap (pixelFormat 3 = first pixel in
 * the low bits of each byte, otherwise in the high bits) */
static inline __attribute__((always_inline)) uint32_t get_px(const struct surf *d, int32_t x, int32_t y)
{
	const uint8_t *row = d->bits + y * d->rowBytes;
	uint32_t dep = d->depth, per, k;

	if (dep == 16)
		return *(const uint16_t *)(row + 2 * x);
	if (dep == 8)
		return row[x];
	per = 8 / dep;
	k = (uint32_t)x % per;
	if (d->fmt == 3)
		return (row[(uint32_t)x / per] >> (k * dep)) & dmask(d);
	return (row[(uint32_t)x / per] >> (8 - dep * (k + 1))) & dmask(d);
}

static inline __attribute__((always_inline)) void put_px(const struct surf *d, int32_t x, int32_t y, uint32_t v)
{
	uint8_t *row = d->bits + y * d->rowBytes;
	uint32_t dep = d->depth, per, k, sh;

	if (dep == 16) {
		*(uint16_t *)(row + 2 * x) = (uint16_t)v;
		return;
	}
	if (dep == 8) {
		row[x] = (uint8_t)v;
		return;
	}
	per = 8 / dep;
	k = (uint32_t)x % per;
	sh = d->fmt == 3 ? k * dep : 8 - dep * (k + 1);
	row[(uint32_t)x / per] = (uint8_t)((row[(uint32_t)x / per] & ~(dmask(d) << sh)) | ((v & dmask(d)) << sh));
}

static uint32_t to_dst(const struct surf *d, uint32_t v565)	/* host-order 565 -> stored halfword */
{
	return d->fmt == 1 ? swap16(v565) : v565;
}

struct pen { uint32_t mode, fg, bg, tx; const uint8_t *pat; uint32_t patKind; };

static void pen_of(const uint8_t *st, const struct surf *d, struct pen *p)
{
	p->mode = st[0];
	p->patKind = st[1];
	/* observed layout on this ROM (see docs §32): bytes +0 transfer mode, +1 pattern, +2 underline mode, +3 font id,
	 * +4 font pointer, +8..+0xF custom pattern rows, +0x10 fore / back / text index bytes, +0x14 / +0x18 / +0x1C
	 * fore / back / text RGBColorType, +0x20 coordinate system, +0x22 flags. */
	p->pat = st + 8;
	if (d->depth == 16) {
		p->fg = to_dst(d, rgb565(st + 0x14));
		p->bg = to_dst(d, rgb565(st + 0x18));
		p->tx = to_dst(d, rgb565(st + 0x1c));
	} else {					/* indexed destination: the index bytes, kept to the depth */
		p->fg = st[0x10] & dmask(d);
		p->bg = st[0x11] & dmask(d);
		p->tx = st[0x12] & dmask(d);
	}
}

static uint32_t pattern_bit(const struct pen *p, const struct surf *d, int32_t x, int32_t y)
{
	static const uint8_t gray[2] = { 0xaa, 0x55 }, light[2] = { 0x88, 0x22 }, dark[2] = { 0xdd, 0x77 };
	uint32_t row, sc = d->density >= 288 ? 4 : d->density >= 144 ? 2 : 1;

	if (p->patKind == 3) {	/* a custom pattern is enlarged with the density (2x2 cells at 144); built-in ones are not */
		x /= (int32_t)sc;
		y /= (int32_t)sc;
	}
	switch (p->patKind) {
	case 1: return 0;
	case 2: row = gray[y & 1]; break;
	case 4: row = light[y & 1]; break;
	case 5: row = dark[y & 1]; break;
	case 3: row = p->pat[y & 7]; break;
	default: return 1;
	}
	return (row >> (7 - (x & 7))) & 1;
}

static void plot_pattern(const struct surf *d, const struct pen *p, int32_t x, int32_t y)
{
	uint32_t bit = pattern_bit(p, d, x, y), cur;

	switch (p->mode) {
	case M_PAINT: put_px(d, x, y, bit ? p->fg : p->bg); break;
	case M_PAINT_INV: put_px(d, x, y, bit ? p->bg : p->fg); break;
	case M_ERASE: if (!bit) put_px(d, x, y, p->bg); break;
	case M_MASK: if (bit) put_px(d, x, y, p->bg); break;
	case M_OVERLAY: if (bit) put_px(d, x, y, p->fg); break;
	case M_INVERT: put_px(d, x, y, (get_px(d, x, y) ^ ~(bit ? p->fg : p->bg)) & dmask(d)); break;
	case M_SWAP:
		cur = get_px(d, x, y);
		if (p->fg != p->bg) {
			if (cur == p->fg)
				put_px(d, x, y, p->bg);
			else if (cur == p->bg)
				put_px(d, x, y, p->fg);
		}
		break;
	default: break;
	}
}

struct clip { int32_t l, t, r, b; };

static void clip_of(const uint8_t *canvas, struct clip *c)
{
	c->l = (int16_t)rd16(canvas + 0);
	c->t = (int16_t)rd16(canvas + 2);
	c->r = c->l + (int16_t)rd16(canvas + 4);
	c->b = c->t + (int16_t)rd16(canvas + 6);
}

static void fill(const struct surf *d, const struct pen *p, const struct clip *c, int32_t l, int32_t t, int32_t r, int32_t b)
{
	int32_t x, y;

	if (l < c->l) l = c->l;
	if (t < c->t) t = c->t;
	if (r > c->r) r = c->r;
	if (b > c->b) b = c->b;
	if (l < 0) l = 0;
	if (t < 0) t = 0;
	if (r > d->w) r = d->w;
	if (b > d->h) b = d->h;
	if (d->depth == 16 && (p->mode == M_PAINT || p->mode == M_OVERLAY) && (p->patKind == 0 || p->patKind > 5)) {
		for (y = t; y < b; y++) {		/* g4u fast path: solid paint/overlay at 16 bpp, same result */
			uint16_t *q = (uint16_t *)(d->bits + y * d->rowBytes) + l;

			for (x = l; x < r; x++)
				*q++ = (uint16_t)p->fg;
		}
		return;
	}
	for (y = t; y < b; y++)
		for (x = l; x < r; x++)
			plot_pattern(d, p, x, y);
}

static int depth_ok(uint32_t dep)
{
	return dep == 1 || dep == 2 || dep == 4 || dep == 8 || dep == 16;
}

/* compressed destinations are accepted only by Bitmap (HALDrawCompressed spec §2); the other primitives refuse
 * them instead of writing raw pixels over the encoded data */
static int setup(const uint8_t *canvas, struct surf *d, struct pen *p, struct clip *c, const char *what, int compressedOk)
{
	const uint8_t *dst = (const uint8_t *)rd32(canvas + 0x0c), *st = (const uint8_t *)rd32(canvas + 0x08);

	if (!dst || !st)
		return 0;
	surf_of(dst, d);
	if (!depth_ok(d->depth) || ((d->flags & 1) && !compressedOk) || !d->bits) {
		static uint32_t warned;

		if (warned++ < 8) {
			dbg_puts("g4s: HALDraw_");
			dbg_puts(what);
			dbg_puts(" to a destination of depth ");
			dbg_hex(d->depth);
			dbg_puts(d->flags & 1 ? " (compressed)" : "");
			dbg_puts(" not drawn\n");
		}
		return 0;
	}
	pen_of(st, d, p);
	clip_of(canvas, c);
	{
		static uint32_t shown;

		if (DRAW_DEBUG && shown++ < 12) {
			dbg_puts("g4r: draw ");
			dbg_puts(what);
			dbg_puts(" mode ");
			dbg_hex(p->mode);
			dbg_puts(" pat ");
			dbg_hex(p->patKind);
			dbg_puts(" state+1c ");
			dbg_hex(rd32(st + 0x1c));
			dbg_puts(" +20 ");
			dbg_hex(rd32(st + 0x20));
			dbg_puts(" +24 ");
			dbg_hex(rd32(st + 0x24));
			dbg_puts(" +28 ");
			dbg_hex(rd32(st + 0x28));
			dbg_puts(" fmt ");
			dbg_hex(d->fmt);
			dbg_puts("\ng4r:   state");
			{
				uint32_t k;

				for (k = 0; k < 0x40; k += 4) {
					dbg_puts(" ");
					dbg_hex(rd32(st + k));
				}
			}
			dbg_puts("\n");
		}
	}
	return 1;
}

/* ---- Rectangle ---- */
static void corner_table(int32_t r, int32_t *b)	/* quarter circle, midpoint algorithm (spec HALDraw §3a) */
{
	int32_t x = 0, y = r, dec = 1 - r;

	b[0] = r;
	while (x < y) {
		if (dec < 0)
			dec += 2 * x + 3;
		else {
			dec += 2 * (x - y) + 5;
			b[y] = x;
			y--;
		}
		x++;
		b[x] = y;
	}
	b[y] = x;
}

void hal_draw_rectangle(const uint8_t *canvas, const int16_t *rect, int32_t radius, int32_t penW)
{
	struct surf d;
	struct pen p;
	struct clip c;
	int32_t L, T, R, B, b[66], j;

	if (!rect || !setup(canvas, &d, &p, &c, "Rectangle", 0))
		return;
	L = rect[0];
	T = rect[1];
	R = L + rect[2];
	B = T + rect[3];
	if (radius > 64)
		radius = 64;
	if (radius <= 0) {
		if (penW <= 0) {
			fill(&d, &p, &c, L, T, R, B);
			return;
		}
		fill(&d, &p, &c, L, T, R, T + penW);
		fill(&d, &p, &c, L, B - penW, R, B);
		fill(&d, &p, &c, L, T + penW, L + penW, B - penW);
		fill(&d, &p, &c, R - penW, T + penW, R, B - penW);
		return;
	}
	for (j = 0; j <= radius + 1; j++)
		b[j] = 0;
	corner_table(radius, b);
	if (penW <= 0) {
		fill(&d, &p, &c, L, T + radius, R, B - radius);
		for (j = 0; j < radius; j++) {
			int32_t o = b[j] < b[j + 1] + 1 ? b[j] : b[j + 1] + 1;

			fill(&d, &p, &c, L + radius - o, T + radius - 1 - j, R - radius + o, T + radius - j);
			fill(&d, &p, &c, L + radius - o, B - radius + j, R - radius + o, B - radius + j + 1);
		}
		return;
	}
	{
		int32_t Q = radius + penW - 1, Ti = T + radius + penW - 1, Bi = B - radius - penW + 1, Li = L + Q, Ri = R - Q, y;

		fill(&d, &p, &c, L, Ti + 1, L + penW, Bi - 1);
		fill(&d, &p, &c, R - penW, Ti + 1, R, Bi - 1);
		for (y = 0; y <= Q; y++) {
			int32_t k = y - (penW - 1), X1 = b[k > 0 ? k : 0] + penW - 1;

			if (y < radius) {
				int32_t X2 = (b[y] < b[y + 1] + 1 ? b[y] : b[y + 1] + 1) - 1;

				fill(&d, &p, &c, Li - X1, Ti - y, Li - X2, Ti - y + 1);
				fill(&d, &p, &c, Ri + X2, Ti - y, Ri + X1, Ti - y + 1);
				fill(&d, &p, &c, Li - X1, Bi + y - 1, Li - X2, Bi + y);
				fill(&d, &p, &c, Ri + X2, Bi + y - 1, Ri + X1, Bi + y);
			} else {
				fill(&d, &p, &c, Li - X1, Ti - y, Ri + X1, Ti - y + 1);
				fill(&d, &p, &c, Li - X1, Bi + y - 1, Ri + X1, Bi + y);
			}
		}
	}
}

/* ---- Line and Pixels ---- */
static int32_t floordiv(int32_t a, int32_t b)
{
	int32_t q = a / b;

	return (a % b && ((a < 0) != (b < 0))) ? q - 1 : q;
}

void hal_draw_line(const uint8_t *canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t penW)
{
	struct surf d;
	struct pen p;
	struct clip c;
	int32_t W, H, m;

	if (penW <= 0 || !setup(canvas, &d, &p, &c, "Line", 0))
		return;
	if (x1 == x2 || y1 == y2) {
		fill(&d, &p, &c, x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2, (x1 > x2 ? x1 : x2) + penW, (y1 > y2 ? y1 : y2) + penW);
		return;
	}
	W = x2 > x1 ? x2 - x1 : x1 - x2;
	H = y2 > y1 ? y2 - y1 : y1 - y2;
	if (W > H) {
		int32_t X0 = x1 < x2 ? x1 : x2, Y0 = x1 < x2 ? y1 : y2, Y1 = x1 < x2 ? y2 : y1, s = Y1 > Y0 ? 1 : -1;

		for (m = 0; m <= W + penW - 1; m++) {
			int32_t Y = Y0 + s * floordiv(2 * H * m + W, 2 * W);

			fill(&d, &p, &c, X0 + m, Y, X0 + m + 1, Y + penW);
		}
	} else {
		int32_t Y0 = y1 < y2 ? y1 : y2, X0 = y1 < y2 ? x1 : x2, X1 = y1 < y2 ? x2 : x1, t = X1 > X0 ? 1 : -1;

		for (m = 0; m <= H + penW - 1; m++) {
			int32_t X = X0 + t * floordiv(2 * W * m + H, 2 * H);

			fill(&d, &p, &c, X, Y0 + m, X + penW, Y0 + m + 1);
		}
	}
}

void hal_draw_pixels(const uint8_t *canvas, uint32_t n, const int16_t *pts, int32_t penW)
{
	struct surf d;
	struct pen p;
	struct clip c, t;
	uint32_t i;

	if (!n || !pts || !setup(canvas, &d, &p, &c, "Pixels", 0))
		return;
	t.l = c.t;					/* reference quirk: clip origin transposed */
	t.t = c.l;
	t.r = c.t + (c.r - c.l);
	t.b = c.l + (c.b - c.t);
	if (c.l != c.t) {
		static uint32_t warned;

		if (warned++ < 4)
			dbg_puts("g4r: HALDraw_Pixels with clip left != top (transposed clip applied)\n");
	}
	for (i = 0; i < n; i++)
		fill(&d, &p, &t, pts[2 * i], pts[2 * i + 1], pts[2 * i] + penW, pts[2 * i + 1] + penW);
}

/* ---- colour lookups for indexed destinations ---- */
static uint32_t rgb_to_index(const uint8_t *rgb, const uint8_t *table)	/* nearest index (HALDrawFindIndexes rule) */
{
	uint8_t rec[4];

	rec[0] = rgb[0];
	rec[1] = rgb[1];
	rec[2] = rgb[2];
	rec[3] = rgb[3];
	hal_draw_find_indexes(1, rec, table);
	return rec[0];
}

static const uint8_t *dst_table(const struct surf *d)	/* destination colour table, the CPO when it has none */
{
	return (d->ctab && rd16(d->ctab)) ? d->ctab : (const uint8_t *)hal_screen_get_colortable();
}

static void expand565(uint32_t v, uint8_t *rec)		/* host-order 565 -> RGBColorType (spec HALDraw §6) */
{
	uint32_t r5 = v >> 11, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;

	rec[0] = 0;
	rec[1] = (uint8_t)(r5 * 8 + (r5 & 7));
	rec[2] = (uint8_t)(g6 * 4 + (g6 & 3));
	rec[3] = (uint8_t)(b5 * 8 + (b5 & 7));
}

/* ---- GetPixel ---- */
uint32_t hal_draw_get_pixel(const uint8_t *bmp, int32_t x, int32_t y, uint32_t asIndex)
{
	struct surf s;
	uint32_t v;
	uint8_t rec[4];

	if (!bmp)
		return 0;
	surf_of(bmp, &s);
	if (!depth_ok(s.depth) || (s.flags & 1) || !s.bits || x < 0 || y < 0 || x >= s.w || y >= s.h)
		return 0;
	v = get_px(&s, x, y);
	if (s.depth != 16) {				/* indexed bitmap: the index, or its colour as RGB565 */
		const uint8_t *t = dst_table(&s);

		if (asIndex)
			return v;
		if (s.depth == 8 && t && v < rd16(t))
			return rgb565(t + 4 + 4 * v);
		std_rgb(std_index(s.depth, v), rec);
		return rgb565(rec);
	}
	if (s.fmt != 2)
		v = swap16(v);
	if (asIndex) {
		expand565(v, rec);
		hal_draw_find_indexes(1, rec, (s.ctab && rd16(s.ctab)) ? s.ctab : 0);
		return rec[0];
	}
	return v;
}

/* ---- Bitmap ---- */

/* HALDrawCompressed spec §2a: encode the source rectangle into the compressed destination's own buffer. The bits
 * pointer sits just after a size prefix of (header size - 0x24) bytes; the buffer holds at least rowBytes * height
 * bytes counted from that prefix. Returns 0 when the destination now holds the encoded rows. */
static int compress_into(const uint8_t *dstBmp, const struct surf *d, const struct surf *s, int32_t fromX, int32_t fromY,
			 int32_t W, int32_t H)
{
	uint32_t prefix = dstBmp[0x0a] >= 0x24 ? dstBmp[0x0a] - 0x24u : 0, type = dstBmp[9] < 2 ? BMP_SCANLINE : dstBmp[0x0d];
	uint32_t cap, n = 0, rb = (uint32_t)d->rowBytes, k;
	uint8_t *rows, *cur, *prev;
	struct surf row;
	int32_t x, y;

	if ((prefix != 2 && prefix != 4) || W > d->w || H > d->h || fromX < 0 || fromY < 0 || fromX + W > s->w || fromY + H > s->h)
		return -1;
	cap = rb * (uint32_t)H - prefix;
	if (prefix == 2 && cap > 65533 - 2)
		cap = 65533 - 2;
	rows = (uint8_t *)pvPortMalloc(2 * rb);
	if (!rows)
		return -1;
	for (k = 0; k < 2 * rb; k++)
		rows[k] = 0;
	row = *d;
	row.rowBytes = (int32_t)rb;
	for (y = 0; y < H; y++) {
		int32_t r;

		cur = rows + (uint32_t)(y & 1) * rb;
		prev = rows + (uint32_t)((y + 1) & 1) * rb;
		row.bits = cur;
		for (x = 0; x < W; x++) {
			uint32_t v = get_px(s, fromX + x, fromY + y);

			if (s->depth == 16 && s->fmt != d->fmt)
				v = swap16(v);
			put_px(&row, x, 0, v);
		}
		r = bmp_pack_row(type, cur, y ? prev : 0, rb, d->depth, d->bits + n, cap - n);
		if (r < 0) {
			vPortFree(rows);
			return -1;
		}
		n += (uint32_t)r;
	}
	vPortFree(rows);
	n += prefix;					/* the size counts the prefix itself; stored little-endian */
	for (k = 0; k < prefix; k++)
		d->bits[(int32_t)k - (int32_t)prefix] = (uint8_t)(n >> (8 * k));
	return 0;
}

/* Length of the encoded data of a compressed descriptor, from the size prefix in front of the bits pointer
 * ((header size - 0x24) bytes; it counts itself). Bitmaps from the ROM store it big-endian, those this DAL
 * compressed (save-behind copies) little-endian, so the value that fits the bitmap is taken. Measured on this ROM:
 * +0x20 matches only for 4-byte prefixes, so it is not used. Falls back to the largest possible encoding. */
static uint32_t encoded_len(const uint8_t *bmp, const struct surf *s)
{
	uint32_t pw = bmp[0x0a] >= 0x24 ? bmp[0x0a] - 0x24u : 0, be = 0, le = 0, k;
	uint32_t max = ((uint32_t)s->rowBytes * 2 + 2) * (uint32_t)(s->h > 0 ? s->h : 0);	/* RLE worst case */

	if (pw != 2 && pw != 4)
		return max;
	for (k = 0; k < pw; k++) {
		be = (be << 8) | s->bits[(int32_t)k - (int32_t)pw];
		le |= (uint32_t)s->bits[(int32_t)k - (int32_t)pw] << (8 * k);
	}
	if (be >= pw && be - pw <= max)
		return be - pw;
	if (le >= pw && le - pw <= max)
		return le - pw;
	return max;
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

uint32_t hal_draw_bitmap(const uint8_t *canvas, const uint8_t *srcBmp, const int16_t *rect, int32_t xOff, int32_t yOff)
{
	struct surf d, s;
	struct pen p;
	struct clip c;
	int32_t L, T, W, H, fromX, fromY, i, j, up = 1, down = 1, small;
	uint32_t mode, tv, lut[256], dm, cacheIn = 0xffffffffu, cacheOut = 0;
	uint8_t *plain = 0, *dstBmp = (uint8_t *)rd32(canvas + 0x0c);

	if (!srcBmp || !rect || !setup(canvas, &d, &p, &c, "Bitmap", 1))
		return 0;
	surf_of(srcBmp, &s);
	L = rect[0];
	T = rect[1];
	W = rect[2];
	H = rect[3];
	if (W <= 0 || H <= 0)
		return 0;
	fromX = L - xOff;
	fromY = T - yOff;
	if (s.density != d.density && !(srcBmp[0x0c] & 1)) {
		if (d.density == 2 * s.density || d.density == 3 * s.density || d.density == 4 * s.density)
			up = (int32_t)(d.density / s.density);
		else if (2 * d.density == s.density)
			down = 2;
	}
	if (d.flags & 1) {				/* compressed destination (HALDrawCompressed spec §2) */
		uint32_t shift = dstBmp[0x0a] >= 0x24 ? dstBmp[0x0a] - 0x24u : 0;

		if (!(s.flags & 1) && s.bits && s.depth == d.depth && p.mode == M_PAINT && L == 0 && T == 0 && up == 1 &&
		    down == 1 && !compress_into(dstBmp, &d, &s, fromX, fromY, W, H))
			return 0;
		d.bits -= shift;			/* otherwise: from now on an ordinary bitmap starting at the prefix */
		d.flags &= ~1u;
		wr32(dstBmp + 0x1c, (uint32_t)d.bits);
		wr16(dstBmp + 6, d.flags);
		{
			static uint32_t shown;

			if (shown++ < 4)
				dbg_puts("g4s: compressed destination turned into a plain bitmap (spec HALDrawCompressed §2b)\n");
		}
	}
	if (s.flags & 1) {				/* compressed source: expand the rows needed into a temporary copy */
		uint32_t type = srcBmp[9] < 2 ? BMP_SCANLINE : srcBmp[0x0d], rows, used = 0, size;
		int32_t last = up > 1 ? floordiv(fromY, up) + (H - 1) / up : down > 1 ? 2 * fromY + 2 * (H - 1) : fromY + H - 1;
		int r;

		if (last < 0 || s.h <= 0)
			return 0;
		rows = (uint32_t)(last + 1 < s.h ? last + 1 : s.h);
		size = (uint32_t)s.rowBytes * rows;
		plain = size ? (uint8_t *)pvPortMalloc(size) : 0;
		r = plain && s.bits ? bmp_unpack(type, s.bits, encoded_len(srcBmp, &s), plain, (uint32_t)s.rowBytes, rows, s.depth,
						 &used) : 99;
		if (r) {
			static uint32_t warned;

			if (warned++ < 8) {
				dbg_puts("g4s: compressed bitmap not drawn: compression type ");
				dbg_hex(type);
				dbg_puts(" result (-1 type, -2 data ends early, -3 malformed) ");
				dbg_hex((uint32_t)r);
				dbg_puts("\n");
			}
			goto done;
		}
		s.bits = plain;
		s.flags &= ~1u;
		s.h = (int32_t)rows;
	} else if (s.bits && s.bits == d.bits) {	/* same pixels (scrolling): read from a snapshot */
		uint32_t size = (uint32_t)s.rowBytes * (uint32_t)(s.h > 0 ? s.h : 0), k;

		plain = size ? (uint8_t *)pvPortMalloc(size) : 0;
		if (!plain)
			return 0;
		for (k = 0; k < size; k++)
			plain[k] = s.bits[k];
		s.bits = plain;
	}
	if (!s.bits || !depth_ok(s.depth)) {
		static uint32_t warned;

		if (warned++ < 16) {
			dbg_puts("g4s: HALDraw_Bitmap source not drawn: depth ");
			dbg_hex(s.depth);
			dbg_puts("\n");
		}
		goto done;
	}
	mode = p.mode;
	if (mode == M_PAINT && (s.flags & 4))
		mode = M_OVERLAY;
	tv = rd32(srcBmp + 0x10);
	dm = dmask(&d);
	small = up == 1 && down == 1 && s.depth == 1 && s.fmt == 0 && (W < 40 || H < 5);
	if (s.depth <= 8) {				/* source index -> stored destination value */
		uint32_t n = 1u << s.depth, k;
		uint8_t rec[4];

		for (k = 0; k < n; k++) {
			if (s.depth == 1 && !s.ctab)
				lut[k] = k ? p.fg : p.bg;
			else if (d.depth == 16)
				lut[k] = s.ctab ? to_dst(&d, rgb565(s.ctab + 4 + 4 * k)) : (std_rgb(std_index(s.depth, k), rec), to_dst(&d, rgb565(rec)));
			else if (s.depth == d.depth && (!s.ctab || !d.ctab))
				lut[k] = k;			/* same depth: indexes copied */
			else {				/* translated by colour (spec HALDraw §5f) */
				if (s.ctab)
					lut[k] = rgb_to_index(s.ctab + 4 + 4 * k, dst_table(&d));
				else {
					std_rgb(std_index(s.depth, k), rec);
					lut[k] = rgb_to_index(rec, dst_table(&d));
				}
				lut[k] &= dm;
			}
		}
	}
	if (s.depth == 16 && d.depth == 16 && s.fmt == d.fmt && mode == M_PAINT && up == 1 && down == 1) {
		for (j = 0; j < H; j++) {		/* g4u fast path: plain 16 bpp copy, same result as the loop below */
			int32_t dy = T + j, sy = fromY + j, x0 = L, x1 = L + W;

			if (dy < 0 || dy >= d.h || sy < 0 || sy >= s.h)
				continue;
			if (x0 < 0) x0 = 0;
			if (x0 - L + fromX < 0) x0 = L - fromX;
			if (x1 > d.w) x1 = d.w;
			if (x1 - L + fromX > s.w) x1 = s.w + L - fromX;
			{
				uint16_t *q = (uint16_t *)(d.bits + dy * d.rowBytes) + x0;
				const uint16_t *src = (const uint16_t *)(s.bits + sy * s.rowBytes) + (x0 - L + fromX);

				for (i = x0; i < x1; i++)
					*q++ = *src++;
			}
		}
		goto done;
	}
	for (j = 0; j < H; j++) {
		int32_t dy = T + j, sy;

		if (dy < 0 || dy >= d.h)
			continue;
		sy = up > 1 ? floordiv(fromY, up) + j / up : down > 1 ? 2 * fromY + 2 * j : fromY + j;
		if (sy < 0 || sy >= s.h)
			continue;
		for (i = 0; i < W; i++) {
			int32_t dx = L + i, sx;
			uint32_t idx = 0, S, isT;

			if (dx < 0 || dx >= d.w)
				continue;
			if (up > 1)
				sx = s.depth == 16 ? floordiv(fromX, up) + i / up : floordiv(fromX + i, up);
			else
				sx = down > 1 ? 2 * fromX + 2 * i : fromX + i;
			if (sx < 0 || sx >= s.w)
				continue;
			if (s.depth == 16) {
				uint32_t v = get_px(&s, sx, sy), host = s.fmt == 1 ? swap16(v) : v;

				isT = host == (tv & 0xffff);
				if (d.depth == 16)
					S = (s.fmt == d.fmt) ? v : swap16(v);
				else {			/* nearest index per pixel (no dithering, see docs §33) */
					if (host != cacheIn) {
						uint8_t rec[4];

						expand565(host, rec);
						cacheIn = host;
						cacheOut = rgb_to_index(rec, dst_table(&d)) & dm;
					}
					S = cacheOut;
				}
			} else {
				idx = get_px(&s, sx, sy);
				S = lut[idx];
				isT = idx == tv;
			}
			if (small) {
				switch (mode) {
				case M_ERASE: case M_MASK: if (idx) put_px(&d, dx, dy, p.bg); continue;
				case M_OVERLAY: if (idx) put_px(&d, dx, dy, tv ? p.bg : p.fg); continue;
				case M_SWAP: continue;
				default: break;
				}
			}
			switch (mode) {
			case M_OVERLAY: if (!isT) put_px(&d, dx, dy, S); break;
			case M_ERASE: if (isT) put_px(&d, dx, dy, p.bg); break;
			case M_MASK: if (!isT) put_px(&d, dx, dy, p.bg); break;
			case M_INVERT: put_px(&d, dx, dy, (get_px(&d, dx, dy) ^ ~S) & dm); break;
			case M_PAINT_INV:
				put_px(&d, dx, dy, s.depth == 16 ? (~S & dm) : lut[((1u << s.depth) - 1) - idx]);
				break;
			default: put_px(&d, dx, dy, S); break;
			}
		}
	}
done:
	if (plain)
		vPortFree(plain);
	return 0;
}

/* ---- Chars ---- */
struct font {
	const uint8_t *strip, *loc;
	uint32_t first, last, rowBytes;
	uint32_t pu;		/* strip pixels per location-table unit (member density / 72) */
	uint32_t scale;		/* destination pixels per strip pixel */
	int32_t fRectH, ascent, descent, leading, exact;
};

/* public FontType v1 / v2 layout (spec HALDraw §4a, member choice §4c); returns 0 if the font cannot be drawn */
static int font_open(const uint8_t *f, uint32_t Dd, uint32_t unscaled, struct font *o)
{
	uint32_t rowWords = rd16(f + 0x18);

	o->first = rd16(f + 2);
	o->last = rd16(f + 4);
	o->fRectH = (int16_t)rd16(f + 0x0e);
	o->ascent = (int16_t)rd16(f + 0x12);
	o->descent = (int16_t)rd16(f + 0x14);
	o->leading = (int16_t)rd16(f + 0x16);
	o->exact = 0;
	if (!(rd16(f) & 0x0200)) {
		o->strip = f + 0x1a;
		o->rowBytes = 2 * rowWords;
		o->loc = o->strip + o->rowBytes * (uint32_t)o->fRectH;
		o->pu = 1;
		o->scale = unscaled ? 1 : Dd / 72;
		o->exact = Dd == 72 || unscaled;
	} else {
		uint32_t n = rd16(f + 0x1c), k, w, pick = 0, found = 0, dens;
		const uint32_t want[5] = { Dd, Dd / 2, Dd / 3, 72, Dd * 3 / 2 };

		if (!n)
			return 0;
		for (w = 0; w < 5 && !unscaled && !found; w++)
			for (k = 0; k < n; k++)
				if (rd16(f + 0x20 + 8 * k) == want[w]) {
					pick = k;
					found = 1;
					break;
				}
		dens = rd16(f + 0x20 + 8 * pick);
		if (dens < 72 || dens % 72)
			return 0;			/* 108 members (1.5x) are not needed on VZ90 */
		o->strip = f + rd32(f + 0x20 + 8 * pick + 4);
		o->loc = f + 0x20 + 8 * n;
		o->rowBytes = ((dens / 36 * rowWords + 1) / 2) * 2;
		o->pu = dens / 72;
		if (unscaled)
			o->scale = 1;
		else if (Dd % dens == 0)
			o->scale = Dd / dens;
		else
			return 0;
		o->exact = dens == Dd;
	}
	if (!o->scale)
		o->scale = 1;
	return 1;
}

void hal_draw_chars(const uint8_t *canvas, int32_t x, int32_t y, const uint8_t *chars, int32_t len, const uint8_t *fontP,
		    const uint8_t *fontMap, uint32_t checkCb, uint32_t r9)
{
	struct surf d;
	struct pen p;
	struct clip c;
	const uint8_t *st = (const uint8_t *)rd32(canvas + 0x08);
	uint32_t Dd, unscaled, sc, k = 0, ul;
	int32_t penX = x, Hc;
	struct font h;

	if (!chars || len <= 0 || !fontP || !setup(canvas, &d, &p, &c, "Chars", 0))
		return;
	Dd = d.density;
	unscaled = (rd16(st + 0x22) >> 1) & 1;
	{
		static uint32_t shown;

		if (DRAW_DEBUG && shown++ < 16) {
			dbg_puts("g4r: Chars x,y ");
			dbg_hex(((uint32_t)x << 16) | ((uint32_t)y & 0xffff));
			dbg_puts(" len ");
			dbg_hex((uint32_t)len);
			dbg_puts(" font ");
			dbg_hex((uint32_t)fontP);
			dbg_puts(" type ");
			dbg_hex(fontMap ? rd16(fontMap) : rd16(fontP));
			dbg_puts(" map ");
			dbg_hex((uint32_t)fontMap);
			dbg_puts(" Dd ");
			dbg_hex(Dd);
			dbg_puts(" clip ");
			dbg_hex(((uint32_t)c.l << 16) | ((uint32_t)c.t & 0xffff));
			dbg_hex(((uint32_t)c.r << 16) | ((uint32_t)c.b & 0xffff));
			dbg_puts(" bits ");
			dbg_hex((uint32_t)d.bits);
			dbg_puts(" wh ");
			dbg_hex(((uint32_t)d.w << 16) | ((uint32_t)d.h & 0xffff));
			dbg_puts(" mode ");
			dbg_hex(p.mode);
			dbg_puts(" tx/bg ");
			dbg_hex((p.tx << 16) | (p.bg & 0xffff));
			dbg_puts("\n");
		}
	}
	sc = unscaled ? 1 : Dd / 72;
	if (fontMap) {					/* the map header supplies the line metrics */
		h.fRectH = (int16_t)rd16(fontMap + 0x0e);
		h.ascent = (int16_t)rd16(fontMap + 0x12);
		h.descent = (int16_t)rd16(fontMap + 0x14);
		h.leading = (int16_t)rd16(fontMap + 0x16);
		h.exact = 0;
	} else if (!font_open(fontP, Dd, unscaled, &h))
		return;
	if (h.exact && (h.ascent || h.descent || h.leading))
		Hc = (int32_t)sc * (h.ascent + h.descent + h.leading);
	else
		Hc = (int32_t)sc * (h.fRectH + h.leading);
	if (y >= c.b || y + (int32_t)sc * (h.fRectH + h.leading) < c.t)
		return;
	while (k < (uint32_t)len && penX < c.r) {
		const uint8_t *fnt = fontP;
		uint32_t code = chars[k++], g, s0, gw, cols;
		int32_t row, col;
		struct font fo;

		if (fontMap) {
			const uint8_t *e = fontMap + 0x1a + 4 * code;

			fnt = ((const uint8_t *const *)fontP)[rd16(e + 2)];
			if (!fnt)
				break;
			if (e[1] == 2 && k < (uint32_t)len) {	/* lead byte: the next byte is the glyph code */
				uint32_t lead = code;

				code = chars[k];
				if (code < rd16(fnt + 2) || code > rd16(fnt + 4)) {
					if (checkCb && (probe_call_with_r9(checkCb, r9, (lead << 8) | code, 0) & 0xff))
						k++;
					code = 0x10000;		/* draws the missing glyph */
				} else
					k++;
			}
		}
		if (!font_open(fnt, Dd, unscaled, &fo))
			break;
		g = (code >= fo.first && code <= fo.last) ? code - fo.first : fo.last - fo.first + 1;
		if (rd16(fo.loc + 2 * (g + 1)) == rd16(fo.loc + 2 * g))
			g = fo.last - fo.first + 1;
		s0 = rd16(fo.loc + 2 * g) * fo.pu;
		gw = (rd16(fo.loc + 2 * (g + 1)) - rd16(fo.loc + 2 * g)) * fo.pu;
		cols = gw * fo.scale;
		for (row = 0; row < Hc; row++) {
			int32_t dy = y + row, srow = row / (int32_t)fo.scale;

			if (dy < c.t || dy >= c.b || dy < 0 || dy >= d.h)
				continue;
			for (col = 0; col < (int32_t)cols; col++) {
				int32_t dx = penX + col;
				uint32_t sbit = s0 + (uint32_t)col / fo.scale, on = 0;

				if (dx < c.l || dx >= c.r || dx < 0 || dx >= d.w)
					continue;
				if (srow < fo.fRectH * (int32_t)fo.pu)
					on = (fo.strip[srow * fo.rowBytes + (sbit >> 3)] >> (7 - (sbit & 7))) & 1;
				switch (p.mode) {
				case M_PAINT: put_px(&d, dx, dy, on ? p.tx : p.bg); break;
				case M_PAINT_INV: put_px(&d, dx, dy, on ? p.bg : p.tx); break;
				case M_OVERLAY: if (on) put_px(&d, dx, dy, p.tx); break;
				case M_ERASE: case M_MASK: if (on) put_px(&d, dx, dy, p.bg); break;
				case M_INVERT: put_px(&d, dx, dy, (get_px(&d, dx, dy) ^ ~(on ? p.tx : p.bg)) & dmask(&d)); break;
				default: break;
				}
			}
		}
		penX += (int32_t)cols;
	}
	ul = st[2];
	if (ul && penX > x) {				/* underline: the last Dd/72 rows of the cell */
		uint32_t t = Dd / 72 ? Dd / 72 : 1, gray = ul == 1;
		int32_t uy, ux, lc = x < c.l ? c.l : x;

		for (uy = y + Hc - (int32_t)t; uy < y + Hc; uy++) {
			if (uy < c.t || uy >= c.b || uy < 0 || uy >= d.h)
				continue;
			for (ux = lc; ux < penX && ux < c.r && ux < d.w; ux++) {
				if (gray && ((uint32_t)(ux + lc - x) % (2 * t)) >= t)
					continue;
				if (get_px(&d, ux, uy) != (p.tx & 0xff))
					put_px(&d, ux, uy, p.fg);
			}
		}
	}
}
