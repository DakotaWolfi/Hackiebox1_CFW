#!/usr/bin/env python3
"""Create or show the boot state file of the SD bootloader.

usage:
  bootstate.py show FILE
  bootstate.py write FILE [--active SLOT] [--next SLOT --kind trial|helper]
                          [--tries N] [--task N] [--helper SLOT]
SLOT is a slot name (ofw1..3, cfw1..3, add1..3) or "none".
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import struct

SLOTS = ["ofw1", "ofw2", "ofw3", "cfw1", "cfw2", "cfw3", "add1", "add2", "add3"]
KINDS = {"none": 0, "trial": 1, "helper": 2}
RESULTS = ["none", "trial confirmed", "trial rolled back", "helper done", "helper failed"]


def crc32_ogg(data):
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def slot_num(name):
    return 0xFF if name == "none" else SLOTS.index(name)


def slot_name(n):
    return "none" if n == 0xFF else SLOTS[n] if n < 9 else "invalid(%d)" % n


def show(path):
    raw = open(path, "rb").read()
    if len(raw) != 32 or raw[:4] != b"NGS1":
        print("invalid: size %d, magic %r" % (len(raw), raw[:4]))
        return
    ok = struct.unpack_from("<I", raw, 28)[0] == crc32_ogg(raw[:28])
    print("crc         %s" % ("ok" if ok else "WRONG"))
    print("active      %s" % slot_name(raw[4]))
    print("next        %s (kind %d, tries %d, task %d)" % (slot_name(raw[5]), raw[6], raw[7], raw[8]))
    print("last result %s" % (RESULTS[raw[9]] if raw[9] < len(RESULTS) else raw[9]))
    print("helper      %s" % slot_name(raw[10]))
    print("rolled back %d" % struct.unpack_from("<I", raw, 12)[0])


def write(a):
    raw = bytearray(32)
    raw[0:4] = b"NGS1"
    raw[4] = slot_num(a.active)
    raw[5] = slot_num(a.next)
    raw[6] = KINDS[a.kind]
    raw[7] = a.tries
    raw[8] = a.task
    raw[10] = slot_num(a.helper)
    struct.pack_into("<I", raw, 28, crc32_ogg(raw[:28]))
    open(a.file, "wb").write(raw)


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("file")
    w = sub.add_parser("write")
    w.add_argument("file")
    w.add_argument("--active", default="cfw1", choices=SLOTS + ["none"])
    w.add_argument("--next", default="none", choices=SLOTS + ["none"])
    w.add_argument("--kind", default="none", choices=KINDS)
    w.add_argument("--tries", type=int, default=0)
    w.add_argument("--task", type=int, default=0)
    w.add_argument("--helper", default="none", choices=SLOTS + ["none"])
    a = p.parse_args()
    if a.cmd == "show":
        show(a.file)
    else:
        write(a)


if __name__ == "__main__":
    main()
