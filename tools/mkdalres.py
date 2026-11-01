#!/usr/bin/env python3
# pv-vz90-dal: write the two small DAL resources a ROM image builder expects next to the DAL code ('amdd' 19000 and
# 'amdi' 19000). Formats as described in docs/INTERFACE.md §20; this DAL does not read either at run time.
# Usage: mkdalres.py <output dir>   -> amdd4A38.bin, amdi4A38.bin
# SPDX-License-Identifier: GPL-3.0-or-later
import os, struct, sys

out = sys.argv[1]
# 'amdd': header word, three segments that each hold only an offset into the above-r9 area and the stream terminator,
# then two empty relocation lists (little-endian counts)
amdd = struct.pack('>I', 0) + b''.join(struct.pack('>i', -16) + b'\0' for _ in range(3)) + struct.pack('<II', 0, 0)
# 'amdi': bytes of module data below r9 (0x28) and above r9 (0x10), big-endian
amdi = struct.pack('>II', 0x28, 0x10)
open(os.path.join(out, 'amdd4A38.bin'), 'wb').write(amdd)
open(os.path.join(out, 'amdi4A38.bin'), 'wb').write(amdi)
