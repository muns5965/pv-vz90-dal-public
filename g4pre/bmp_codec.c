/* pv-vz90-dal g4s: compression and expansion of Palm OS bitmap data (plain rows <-> encoded stream).
 * Written from the public description of the three Palm OS bitmap compression types (Palm OS Programmer's
 * Companion / Reference, BitmapCompressionType) and checked against data observed in this ROM and against
 * independent test vectors (tools/test_bmp_codec.py). Pure function: no OS or hardware access.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bmp_codec.h"

/* All three decoders read at most inLen input bytes and fill exactly total = rowBytes * rows output bytes. They
 * return BMP_OK, BMP_ERR_SHORT when the input ends before the output is complete, or BMP_ERR_BAD for a malformed
 * stream. Output beyond what was requested is never written; a run that continues past the last requested row is
 * normal (the caller may ask for fewer rows than were encoded). */

/* ScanLine: each row is cut into groups of up to 8 bytes; a flag byte comes first, its bits (most significant
 * first) tell for each byte of the group whether a new byte follows in the data (1) or the byte is the same as in
 * the row above (0). The first row has no row above; its unflagged bytes are taken as 0. */
static int scanline(const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t rowBytes, uint32_t rows, uint32_t *used)
{
	uint32_t y, x, bit, i = 0;

	for (y = 0; y < rows; y++) {
		uint8_t *row = out + y * rowBytes;
		const uint8_t *above = y ? row - rowBytes : 0;

		for (x = 0; x < rowBytes; x += 8) {
			uint32_t flags;

			if (i >= inLen)
				return BMP_ERR_SHORT;
			flags = in[i++];
			for (bit = 0; bit < 8 && x + bit < rowBytes; bit++) {
				if (flags & (0x80u >> bit)) {
					if (i >= inLen)
						return BMP_ERR_SHORT;
					row[x + bit] = in[i++];
				} else
					row[x + bit] = above ? above[x + bit] : 0;
			}
		}
	}
	*used = i;
	return BMP_OK;
}

/* RLE: pairs of (count, value byte), count 1..255, filling the rows one after another. */
static int rle(const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t total, uint32_t *used)
{
	uint32_t n = 0, i = 0;

	while (n < total) {
		uint32_t count, value;

		if (i + 2 > inLen)
			return BMP_ERR_SHORT;
		count = in[i];
		value = in[i + 1];
		i += 2;
		if (!count)				/* never produced by an encoder */
			return BMP_ERR_BAD;
		while (count-- && n < total)
			out[n++] = (uint8_t)value;
	}
	*used = i;
	return BMP_OK;
}

/* PackBits: a signed control byte c; 0..127 means c + 1 literal units follow, -1..-127 means the next unit is
 * repeated 1 - c times, -128 is skipped. A unit is one byte, or one 16-bit pixel (two bytes, kept in stored order)
 * for 16 bpp bitmaps. */
static int packbits(const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t total, uint32_t unit, uint32_t *used)
{
	uint32_t n = 0, i = 0, k;

	while (n < total) {
		int32_t c;

		if (i >= inLen)
			return BMP_ERR_SHORT;
		c = (int8_t)in[i++];
		if (c == -128)
			continue;
		if (c >= 0) {
			uint32_t len = (uint32_t)(c + 1) * unit;

			if (i + len > inLen)
				return BMP_ERR_SHORT;
			for (k = 0; k < len && n < total; k++)
				out[n++] = in[i + k];
			i += len;
		} else {
			uint32_t times = (uint32_t)(1 - c);

			if (i + unit > inLen)
				return BMP_ERR_SHORT;
			while (times-- && n < total)
				for (k = 0; k < unit && n < total; k++)
					out[n++] = in[i + k];
			i += unit;
		}
	}
	*used = i;
	return BMP_OK;
}

int bmp_unpack(uint32_t type, const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t rowBytes, uint32_t rows,
	       uint32_t depth, uint32_t *usedOut)
{
	uint32_t used = 0;
	int r;

	switch (type) {
	case BMP_SCANLINE: r = scanline(in, inLen, out, rowBytes, rows, &used); break;
	case BMP_RLE: r = rle(in, inLen, out, rowBytes * rows, &used); break;
	case BMP_PACKBITS: r = packbits(in, inLen, out, rowBytes * rows, depth == 16 ? 2 : 1, &used); break;
	default: r = BMP_ERR_TYPE; break;
	}
	if (usedOut)
		*usedOut = used;
	return r;
}

/* ---- encoding, one row at a time (g4s) ----
 * Each function appends the encoding of one plain row to out (at most cap bytes) and returns the number of bytes
 * written, or -1 when the row does not fit. The row is never split. */
static int32_t pack_scanline(const uint8_t *row, const uint8_t *above, uint32_t rowBytes, uint8_t *out, uint32_t cap)
{
	uint32_t x, bit, n = 0;

	for (x = 0; x < rowBytes; x += 8) {
		uint32_t flagPos = n, flags = 0;

		if (n >= cap)
			return -1;
		n++;
		for (bit = 0; bit < 8 && x + bit < rowBytes; bit++) {
			if (!above || row[x + bit] != above[x + bit]) {
				if (n >= cap)
					return -1;
				flags |= 0x80u >> bit;
				out[n++] = row[x + bit];
			}
		}
		out[flagPos] = (uint8_t)flags;
	}
	return (int32_t)n;
}

static int32_t pack_rle(const uint8_t *row, uint32_t rowBytes, uint8_t *out, uint32_t cap)
{
	uint32_t i = 0, n = 0;

	while (i < rowBytes) {
		uint32_t run = 1;

		while (i + run < rowBytes && run < 255 && row[i + run] == row[i])
			run++;
		if (n + 2 > cap)
			return -1;
		out[n++] = (uint8_t)run;
		out[n++] = row[i];
		i += run;
	}
	return (int32_t)n;
}

static int same_unit(const uint8_t *a, const uint8_t *b, uint32_t unit)
{
	return a[0] == b[0] && (unit == 1 || a[1] == b[1]);
}

static int32_t pack_packbits(const uint8_t *row, uint32_t rowBytes, uint32_t unit, uint8_t *out, uint32_t cap)
{
	uint32_t units = rowBytes / unit, i = 0, n = 0, k;

	while (i < units) {
		uint32_t run = 1;

		while (i + run < units && run < 128 && same_unit(row + (i + run) * unit, row + i * unit, unit))
			run++;
		if (run >= 2) {				/* a repeat: control 1 - run, then the unit */
			if (n + 1 + unit > cap)
				return -1;
			out[n++] = (uint8_t)(1 - (int32_t)run);
			for (k = 0; k < unit; k++)
				out[n++] = row[i * unit + k];
			i += run;
			continue;
		}
		run = 1;				/* literals up to the next pair of equal units */
		while (i + run < units && run < 128 &&
		       !(i + run + 1 < units && same_unit(row + (i + run) * unit, row + (i + run + 1) * unit, unit)))
			run++;
		if (n + 1 + run * unit > cap)
			return -1;
		out[n++] = (uint8_t)(run - 1);
		for (k = 0; k < run * unit; k++)
			out[n++] = row[i * unit + k];
		i += run;
	}
	return (int32_t)n;
}

int32_t bmp_pack_row(uint32_t type, const uint8_t *row, const uint8_t *above, uint32_t rowBytes, uint32_t depth,
		     uint8_t *out, uint32_t cap)
{
	switch (type) {
	case BMP_SCANLINE: return pack_scanline(row, above, rowBytes, out, cap);
	case BMP_RLE: return pack_rle(row, rowBytes, out, cap);
	case BMP_PACKBITS: return pack_packbits(row, rowBytes, depth == 16 ? 2 : 1, out, cap);
	default: return -1;
	}
}
