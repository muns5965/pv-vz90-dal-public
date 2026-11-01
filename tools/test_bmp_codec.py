#!/usr/bin/env python3
"""Host test for g4pre/bmp_codec.c: (1) encodes random bitmaps with independent Python encoders written from the
public format descriptions and expands them with the C code; (2) encodes with the C code and expands with
independent Python decoders. Both directions must give back the original pixels. (3) truncated encodings must be
reported as short input and random bytes must never crash or read past the input (built with ASan/UBSan).
usage: python3 tools/test_bmp_codec.py   (needs a host C compiler)
SPDX-License-Identifier: GPL-3.0-or-later"""
import os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'g4pre')


def enc_scanline(data, rb, rows):
    out = bytearray()
    for y in range(rows):
        row = data[y * rb:(y + 1) * rb]
        above = data[(y - 1) * rb:y * rb] if y else bytes(rb)
        for x in range(0, rb, 8):
            flags, lit = 0, bytearray()
            for b in range(min(8, rb - x)):
                if y == 0 or row[x + b] != above[x + b]:
                    flags |= 0x80 >> b
                    lit.append(row[x + b])
            out.append(flags)
            out += lit
    return bytes(out)


def enc_rle(data):
    out, i = bytearray(), 0
    while i < len(data):
        j = i
        while j < len(data) and j - i < 255 and data[j] == data[i]:
            j += 1
        out += bytes([j - i, data[i]])
        i = j
    return bytes(out)


def enc_packbits(data, rb, rows, unit):
    out = bytearray()
    for y in range(rows):
        row = data[y * rb:(y + 1) * rb]
        u = [row[k:k + unit] for k in range(0, rb, unit)]
        i = 0
        while i < len(u):
            j = i
            while j < len(u) and j - i < 128 and u[j] == u[i]:
                j += 1
            if j - i >= 2:
                out.append((1 - (j - i)) & 0xff)
                out += u[i]
                i = j
                continue
            j = i
            while j < len(u) and j - i < 128 and (j + 1 >= len(u) or u[j] != u[j + 1]):
                j += 1
            j = max(j, i + 1)
            out.append(j - i - 1)
            for k in range(i, j):
                out += u[k]
            i = j
        out.append(0x80)  # a no-op control byte between rows must be skipped
    return bytes(out)


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "bmp_codec.h"
int main(int c, char **v) {
    unsigned type = atoi(v[2]), rb = atoi(v[3]), rows = atoi(v[4]), depth = atoi(v[5]), y, n = 0, len, used;
    static unsigned char buf[1 << 20], out[1 << 21];
    unsigned char *in;
    FILE *f = fopen(v[6], "rb"); len = fread(buf, 1, sizeof buf, f); fclose(f);
    (void)c;
    in = malloc(len ? len : 1);                 /* exactly the input length: the sanitizer catches over-reads */
    for (y = 0; y < len; y++) in[y] = buf[y];
    if (v[1][0] == 'd') {
        int r = bmp_unpack(type, in, len, out, rb, rows, depth, &used);
        if (r == BMP_ERR_TYPE) return 2;
        if (r == BMP_ERR_SHORT) return 4;
        if (r == BMP_ERR_BAD) return 5;
        if (used > len) return 6;
        fwrite(out, 1, rb * rows, stdout); return 0;
    }
    for (y = 0; y < rows; y++) {
        int r = bmp_pack_row(type, in + y * rb, y ? in + (y - 1) * rb : 0, rb, depth, out + n, sizeof out - n);
        if (r < 0) return 2;
        n += r;
    }
    fwrite(out, 1, n, stdout); return 0;
}
"""


def dec_scanline(b, rb, rows):
    out, i = bytearray(rb * rows), 0
    for y in range(rows):
        for x in range(0, rb, 8):
            fl = b[i]; i += 1
            for k in range(min(8, rb - x)):
                if fl & (0x80 >> k):
                    out[y * rb + x + k] = b[i]; i += 1
                elif y:
                    out[y * rb + x + k] = out[(y - 1) * rb + x + k]
    return bytes(out)


def dec_rle(b, total):
    out, i = bytearray(), 0
    while len(out) < total:
        out += bytes([b[i + 1]]) * b[i]; i += 2
    return bytes(out[:total])


def dec_packbits(b, total, unit):
    out, i = bytearray(), 0
    while len(out) < total:
        c = b[i] - 256 if b[i] > 127 else b[i]; i += 1
        if c == -128:
            continue
        if c >= 0:
            out += b[i:i + (c + 1) * unit]; i += (c + 1) * unit
        else:
            out += b[i:i + unit] * (1 - c); i += unit
    return bytes(out[:total])


def main():
    os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
    tmp = tempfile.mkdtemp()
    h = os.path.join(tmp, 'h.c')
    open(h, 'w').write(HARNESS)
    exe = os.path.join(tmp, 't')
    subprocess.check_call(['cc', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', SRC, h,
                           os.path.join(SRC, 'bmp_codec.c'), '-o', exe])
    rnd, n, fails = random.Random(1), 0, 0
    for trial in range(300):
        depth = rnd.choice([1, 2, 4, 8, 16])
        w = rnd.randint(1, 340)
        rb = ((w * depth + 15) // 16) * 2
        rows = rnd.randint(1, 40)
        pal = [bytes([rnd.randrange(256)]) for _ in range(rnd.randint(1, 4))]
        data = bytearray()
        while len(data) < rb * rows:  # runs and noise, like icons
            data += rnd.choice(pal) * rnd.randint(1, 30) if rnd.random() < .7 else bytes([rnd.randrange(256)])
        data = bytes(data[:rb * rows])
        cases = [(0, enc_scanline(data, rb, rows)), (1, enc_rle(data)),
                 (2, enc_packbits(data, rb, rows, 2 if depth == 16 else 1))]
        p = os.path.join(tmp, 'in.bin')
        for t, blob in cases:
            open(p, 'wb').write(blob)
            got = subprocess.run([exe, 'd', str(t), str(rb), str(rows), str(depth), p], capture_output=True).stdout
            n += 1
            if got != data:
                fails += 1
                print('FAIL decode type %d depth %d rb %d rows %d' % (t, depth, rb, rows))
        for t, blob in cases:                   # cut input: must be reported, never read past the end
            cut = rnd.randrange(0, len(blob)) if blob else 0
            open(p, 'wb').write(blob[:cut])
            r = subprocess.run([exe, 'd', str(t), str(rb), str(rows), str(depth), p], capture_output=True)
            n += 1
            if r.returncode != 4 and not (r.returncode == 0 and r.stdout == data):   # only no-op bytes cut
                fails += 1
                print('FAIL short input not reported: type %d cut %d of %d rc %d %s' % (t, cut, len(blob), r.returncode, r.stderr[:200]))
        noise = bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 64)))
        for t in (0, 1, 2):                      # random bytes: any result but no crash / over-read
            open(p, 'wb').write(noise)
            r = subprocess.run([exe, 'd', str(t), str(rb), str(rows), str(depth), p], capture_output=True)
            n += 1
            if r.returncode not in (0, 4, 5):
                fails += 1
                print('FAIL noise input: type %d rc %d %s' % (t, r.returncode, r.stderr[:200]))
        open(p, 'wb').write(data)
        unit = 2 if depth == 16 else 1
        for t, dec in ((0, lambda b: dec_scanline(b, rb, rows)), (1, lambda b: dec_rle(b, rb * rows)),
                       (2, lambda b: dec_packbits(b, rb * rows, unit))):
            r = subprocess.run([exe, 'e', str(t), str(rb), str(rows), str(depth), p], capture_output=True)
            n += 1
            if r.returncode or dec(r.stdout) != data:
                fails += 1
                print('FAIL encode type %d depth %d rb %d rows %d' % (t, depth, rb, rows))
    bad = subprocess.run([exe, 'd', '3', '4', '1', '8', p], capture_output=True).returncode
    print('%d cases, %d failures; unknown type -> %s' % (n, fails, 'rejected' if bad == 2 else 'NOT rejected'))
    sys.exit(1 if fails or bad != 2 else 0)


main()
