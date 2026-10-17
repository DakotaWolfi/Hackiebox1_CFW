# Variants of the wear-levelling state of an image made by wl_fatfsgen.py,
# for the check of assets.c. Usage: state.py in out KIND, KIND one of
#   copy1   the first copy broken (the driver repairs it from the second)
#   both    both copies broken (the driver would format the partition)
#   v1      both copies valid with version 1 (the driver would format)
#   maxpos  both copies valid with another layout
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys
src, dst, kind = sys.argv[1], sys.argv[2], sys.argv[3]
d = bytearray(open(src, "rb").read())
S = 4096
size = len(d)
state_size = max(S, (64 + size // S * 16 + S - 1) // S * S)
s1, s2 = size - 2 * state_size - S, size - state_size - S
def crc(b):
    # the ROM's crc32_le with the start value 0xFFFFFFFF
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    return c ^ 0xFFFFFFFF
def change(at, field, value):
    struct.pack_into("<I", d, at + 4 * field, value)
    struct.pack_into("<I", d, at + 60, crc(d[at:at + 60]))
if kind == "copy1":
    d[s1 + 60] ^= 0xFF
elif kind == "both":
    d[s1 + 60] ^= 0xFF
    d[s2 + 60] ^= 0xFF
elif kind == "v1":
    change(s1, 6, 1)
    change(s2, 6, 1)
elif kind == "maxpos":
    v = struct.unpack_from("<I", d, s1 + 4)[0] - 1
    change(s1, 1, v)
    change(s2, 1, v)
else:
    sys.exit("unknown kind")
open(dst, "wb").write(d)
