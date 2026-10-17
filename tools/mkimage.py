#!/usr/bin/env python3
"""Turn the raw image into a slot file.

The build block is already the end of the raw image; this checks it and
appends the SHA-256 digest of everything before as 64 lower-case hex digits.

usage: mkimage.py RAW.bin SLOT.bin
SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import struct
import sys

MAGIC = 0xBEAC0005
SLOT_MAX = 212992


def main(raw_path, out_path):
    data = open(raw_path, "rb").read()
    if len(data) < 112:
        sys.exit("image too short")
    bb = data[-112:]
    m1, _, build_time, debug = struct.unpack_from("<IIII", bb, 0)
    m2 = struct.unpack_from("<I", bb, 108)[0]
    if m1 != MAGIC or m2 != MAGIC:
        sys.exit("build block is not the end of the image")
    if debug != 0 or bb[92] & 7:
        sys.exit("debug flag or slot binding set")
    if bb[93:97] != b"CFW1":
        sys.exit("family marker missing")
    if struct.unpack_from("<I", bb, 100)[0] != 2:
        sys.exit("image kind is not 2")
    # reserved bytes and the bootloader word zero.
    if struct.unpack_from("<I", bb, 4)[0] != 0 or bb[92] & 0xF8 or any(bb[97:100]):
        sys.exit("reserved build block bytes not zero")
    if struct.unpack_from("<I", bb, 104)[0] != 0:
        sys.exit("bootloader word is not zero")
    if bb[16 + 7] != 0 or bb[24 + 60] != 0:
        sys.exit("revision or build date not NUL-terminated")
    digest = hashlib.sha256(data).hexdigest().encode()
    out = data + digest
    if len(out) > SLOT_MAX:   # the trailer counts
        sys.exit("slot file of %d bytes exceeds %d" % (len(out), SLOT_MAX))
    open(out_path, "wb").write(out)
    print("%s: %d bytes (%d free), build %d" % (out_path, len(out), SLOT_MAX - len(out), build_time))


if __name__ == "__main__":
    main(*sys.argv[1:])
