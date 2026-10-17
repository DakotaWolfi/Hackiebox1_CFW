#!/usr/bin/env python3
"""Host tool: checks the header, page layout and audio stream of a content file.
usage: check.py <file.taf>
SPDX-License-Identifier: GPL-3.0-or-later
"""
import struct, sys

def crc(page):
    c = 0
    for i, b in enumerate(page):
        if 22 <= i < 26:
            b = 0
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c

d = open(sys.argv[1], "rb").read()
assert struct.unpack(">I", d[:4])[0] == 4092
serial = None
problems = 0
for off in range(4096, len(d), 4096):
    blk = d[off:off + 4096]
    pos = 0
    while pos < len(blk):
        pg = blk[pos:]
        assert pg[:4] == b"OggS", (off, pos)
        segs = pg[26]
        size = 27 + segs + sum(pg[27:27 + segs])
        assert pos + size <= len(blk), "page crosses block"
        seq = struct.unpack("<I", pg[18:22])[0]
        ser = struct.unpack("<I", pg[14:18])[0]
        serial = serial or ser
        ok_seq = seq in (0, 1) and off == 4096 or seq == off // 4096 + 1
        if not ok_seq or ser != serial or crc(pg[:size]) != struct.unpack("<I", pg[22:26])[0]:
            problems += 1
            print("bad page", off, pos, seq)
        pos += size
print("blocks", (len(d) - 4096 + 4095) // 4096, "problems", problems, "serial 0x%08x" % serial)
