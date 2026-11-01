#!/usr/bin/env python3
# pv-vz90-dal: build a 64 KiB stand-alone PV TEST image (not a Palm OS ROM builder): code at offset 0, ROM card header at 0x18000 so that CloudpilotEmu's RomInfo5
# (src/common/rom_info5.cpp) recognises it as a PV ROM: +8 signature 0xfeedbeef, +0x10 card name, +0x30 manufacturer,
# +0x80 company id, +0x84 HAL id 'PvPv', +0x8c ROM name (NUL-terminated strings, max 32 bytes).
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys

def fourcc(s):  # multichar constant as the C compiler builds it ('PvPv' -> 0x50765076), stored little endian
    v = 0
    for ch in s.encode():
        v = (v << 8) | ch
    return v

code = open(sys.argv[1], 'rb').read()
if len(code) > 0x18000:
    sys.exit('code larger than 96 KiB')
extra = open(sys.argv[3], 'rb').read() if len(sys.argv) > 3 else b''  # optional local-only module at +0x20000
img = bytearray(0x20000 + ((len(extra) + 0xffff) & ~0xffff))
img[:len(code)] = code
h = 0x18000
struct.pack_into('<I', img, h + 8, 0xfeedbeef)
for off, s in ((0x10, b'pv-vz90-dal'), (0x30, b'pv-vz90-dal project'), (0x8c, b'stage g1')):
    img[h + off:h + off + len(s) + 1] = s + b'\0'
struct.pack_into('<II', img, h + 0x80, fourcc('none'), fourcc('PvPv'))
img[0x20000:0x20000 + len(extra)] = extra
# module marker inside the always-present header area: 'MODL', module offset, module size (0 = no module)
struct.pack_into('<4sII', img, h + 0x100, b'MODL', 0x20000 if extra else 0, len(extra))
open(sys.argv[2], 'wb').write(img)
