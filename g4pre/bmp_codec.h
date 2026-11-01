/* pv-vz90-dal g4s: compressed bitmap encoding and expansion. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BMP_UNPACK_H
#define BMP_UNPACK_H
#include <stdint.h>
enum { BMP_SCANLINE = 0, BMP_RLE = 1, BMP_PACKBITS = 2 };
enum { BMP_OK = 0, BMP_ERR_TYPE = -1, BMP_ERR_SHORT = -2, BMP_ERR_BAD = -3 };
/* Expands the first rows * rowBytes bytes of plain pixel data from the compressed data at in, reading at most inLen
 * bytes. Returns BMP_OK, BMP_ERR_TYPE (unsupported type), BMP_ERR_SHORT (input ended first) or BMP_ERR_BAD. */
int bmp_unpack(uint32_t type, const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t rowBytes, uint32_t rows,
	       uint32_t depth, uint32_t *usedOut);	/* usedOut (may be NULL): input bytes consumed on success */
/* Appends the encoding of one plain row (above = the previous plain row, NULL for the first) to out, using at most
 * cap bytes. Returns the byte count, or -1 if it does not fit or the type is not supported. */
int32_t bmp_pack_row(uint32_t type, const uint8_t *row, const uint8_t *above, uint32_t rowBytes, uint32_t depth,
		     uint8_t *out, uint32_t cap);
#endif
