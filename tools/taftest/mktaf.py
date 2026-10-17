#!/usr/bin/env python3
"""Host tool: builds a test content file from the
Ogg stream mkogg writes: header block with SHA-1, num_bytes, audio_id and the
chapter page numbers (default one chapter at page 0), filled to
4096 bytes with field 5.

usage: mktaf.py <in.ogg> <out.taf> <audio_id> [page,page,...]
SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib, struct, sys

def varint(v):
    out = b""
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])

def field(num, wire, payload):
    return varint(num << 3 | wire) + payload

ogg = open(sys.argv[1], "rb").read()
audio_id = int(sys.argv[3], 0)
msg = field(1, 2, varint(20) + hashlib.sha1(ogg).digest())
msg += field(2, 0, varint(len(ogg)))
msg += field(3, 0, varint(audio_id))
chapters = [int(p) for p in sys.argv[4].split(",")] if len(sys.argv) > 4 else [0]
msg += field(4, 2, varint(len(b"".join(varint(p) for p in chapters))) + b"".join(varint(p) for p in chapters))
# Field 5 fills the rest of the 4092 bytes; its length prefix size depends
# on its length, so search for the fitting length.
for n in range(4092 - len(msg), 0, -1):
    fill = field(5, 2, varint(n) + b"\0" * n)
    if len(msg) + len(fill) == 4092:
        msg += fill
        break
assert len(msg) == 4092
open(sys.argv[2], "wb").write(struct.pack(">I", 4092) + msg + ogg)
