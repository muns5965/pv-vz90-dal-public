#!/usr/bin/env python3
# pv-vz90-dal: pack the full-ROM DAL ('boot' code, 'amdd', 'amdi') into one bundle file for an emulator-side ROM loader.
# The link address is verified here, at build time, from the ELF and the linker symbols: the loader only checks the
# hashes and the build ID recorded in the bundle (docs/INTERFACE.md §38).
# Usage: mkbundle.py <dal-fullrom.elf> <dal-fullrom.bin> <amdd.bin> <amdi.bin> <config.stamp> <out.bundle>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bundle format 1 (all integers little-endian), header 0x100 bytes:
#   0x00  8  magic "VZ90DAL\0"
#   0x08  4  format version (1)
#   0x0C  4  header size (0x100)
#   0x10  4  link address of 'boot' (0x10000230)
#   0x14  4  resource count (3)
#   0x18 20  build ID: git commit (SHA-1, 20 bytes; zero when unknown)
#   0x2C  4  flags: bit 0 working tree had uncommitted changes, bit 1 DAL_LOG build, bit 2 DAL_SELFTEST build
#   0x30 48  resource table, 3 entries of 16 bytes in the order boot, amdd, amdi:
#              +0 type (4 ASCII bytes, e.g. "boot"), +4 resource ID (2), +6 reserved (2, zero), +8 offset (4), +12 length (4)
#   0x60 96  SHA-256 of each resource, same order
#   0xC0 32  SHA-256 of the whole bundle, computed with these 32 bytes set to zero
#   0xE0 32  reserved (zero)
# Resources follow the header back to back from 0x100, each starting on a 4-byte boundary (zero padding between);
# the file ends at the end of the last resource.
import hashlib, re, struct, subprocess, sys

MAGIC, VERSION, HDR, LINK = b'VZ90DAL\0', 1, 0x100, 0x10000230
RES_ID = 0x4A38


def die(msg):
    sys.exit('mkbundle: ' + msg)


def run(*cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        die('%s failed: %s' % (cmd[0], p.stderr.strip()))
    return p.stdout


def verify_link(elf, binary):
    hdr = run('arm-none-eabi-readelf', '-h', elf)
    m = re.search(r'Entry point address:\s+0x([0-9a-f]+)', hdr)
    if not m or int(m.group(1), 16) != LINK:
        die('ELF entry point is not 0x%08x' % LINK)
    syms = {}
    for line in run('arm-none-eabi-nm', elf).splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    if syms.get('_start') != LINK:
        die('_start is not at 0x%08x' % LINK)
    # .text must begin at the link address, no loaded section may lie below it in the ROM window, and the raw image
    # must run exactly from the link address to the end of the .data load image (.ARM.exidx may sit in between)
    text = data = None
    rom_end = LINK
    for line in run('arm-none-eabi-readelf', '-SW', elf).splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)\s+[0-9a-f]{2}\s+([A-Za-z]*)\s', line)
        if not m:
            continue
        name, addr, size, flg = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        if name == '.text':
            text = (addr, size)
        if name == '.data':
            data = size
        if 'A' in flg and 0x10000000 <= addr < 0x20000000:
            if addr < LINK:
                die('section %s at 0x%08x lies below the link address' % (name, addr))
            rom_end = max(rom_end, addr + size)
    if not text or text[0] != LINK:
        die('.text does not start at 0x%08x' % LINK)
    load = syms.get('__data_load')
    if load is None or load < rom_end:
        die('.data load image overlaps the code')
    expect = load + (data or 0) - LINK
    if len(binary) != expect:
        die('raw DAL image size %d does not match the linked range (%d)' % (len(binary), expect))


def build_id():
    try:
        head = subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], capture_output=True,
                               text=True, check=True).stdout.strip() != ''
        return bytes.fromhex(head), dirty
    except Exception:
        return b'\0' * 20, True


def main():
    if len(sys.argv) != 7:
        die('usage: mkbundle.py <elf> <bin> <amdd> <amdi> <config.stamp> <out>')
    elf, binp, amddp, amdip, stamp, out = sys.argv[1:]
    boot = open(binp, 'rb').read()
    verify_link(elf, boot)
    res = [(b'boot', boot), (b'amdd', open(amddp, 'rb').read()), (b'amdi', open(amdip, 'rb').read())]
    cfg = open(stamp).read()
    bid, dirty = build_id()
    flags = (1 if dirty else 0) | (2 if '-DDAL_LOG=1' in cfg else 0) | (4 if '-DDAL_SELFTEST=1' in cfg else 0)

    body, table, hashes, pos = b'', b'', b'', HDR
    for tag, data in res:
        pad = (-pos) % 4
        body += b'\0' * pad
        pos += pad
        table += tag + struct.pack('<HHII', RES_ID, 0, pos, len(data))
        hashes += hashlib.sha256(data).digest()
        body += data
        pos += len(data)
    hdr = MAGIC + struct.pack('<IIII', VERSION, HDR, LINK, len(res)) + bid + struct.pack('<I', flags) + table + hashes
    hdr += b'\0' * 32 + b'\0' * 32
    assert len(hdr) == HDR
    img = bytearray(hdr + body)
    img[0xC0:0xE0] = hashlib.sha256(bytes(img)).digest()
    open(out, 'wb').write(img)
    print('bundle %s: %d bytes, build %s%s, flags %#x, sha256 %s' % (out, len(img), bid.hex()[:12],
          ' (dirty)' if dirty else '', flags, hashlib.sha256(img).hexdigest()[:16]))


if __name__ == '__main__':
    main()
