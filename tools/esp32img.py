#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""App images of the ESP32 box.

  esp32img.py check IMAGE            chip, flash settings, size, checksums,
                                     project, version, server block and build
                                     date; exit status 1 on a failure
  esp32img.py stamp IMAGE BUILD_H    build date and time of the app description
                                     set to CFW_BUILD_TIME of the header (UTC),
                                     checksum and SHA-256 fixed;
                                     the post-build step of esp32/CMakeLists.txt
  esp32img.py body IMAGE OUT [--host H] [--port P]
                                     the body of an answer to /v1/ota/3: the image, with the server
                                     block filled in when --host is given,
                                     checksum and appended SHA-256 recomputed,
                                     then the SHA-256 as 64 lowercase hex
                                     characters
  esp32img.py info BODY_OR_IMAGE     project, version, server block

Image format: ESP-IDF documentation, "App Image Format".
"""
import calendar
import hashlib
import re
import struct
import sys
import time

MAGIC = 0xE9
CHIP_ESP32S3 = 9
SLOT = 0x360000
APP_DESC_MAGIC = 0xABCD5432
MARKER = b"CFWSRV" + b"01"  # never one literal: this file may end up in an image search
PROJECT_CFW = "toniebox-cfw-esp32"
PROJECT_OFW = "toniebox-esp32-eu"


class ImageError(Exception):
    pass


def layout(d):
    """The end of the segments, the checksum's offset, the end of the image
    (with the appended hash when there is one)."""
    if len(d) < 24 or d[0] != MAGIC:
        raise ImageError("no app image (magic)")
    n = d[1]
    off = 24
    for _ in range(n):
        if off + 8 > len(d):
            raise ImageError("segment header beyond the end")
        _, length = struct.unpack_from("<II", d, off)
        off += 8 + length
        if off > len(d):
            raise ImageError("segment beyond the end")
    seg_end = off
    csum_at = (off + 16) // 16 * 16 - 1  # padding to 16 bytes, checksum last
    end = csum_at + 1 + (32 if d[23] == 1 else 0)
    if end > len(d):
        raise ImageError("image shorter than its segments")
    return n, seg_end, csum_at, end


def checksum(d, n):
    c = 0xEF
    off = 24
    for _ in range(n):
        _, length = struct.unpack_from("<II", d, off)
        for b in d[off + 8: off + 8 + length]:
            c ^= b
        off += 8 + length
    return c


def app_desc(d):
    # the first segment starts with the app description
    magic = struct.unpack_from("<I", d, 32)[0]
    if magic != APP_DESC_MAGIC:
        raise ImageError("no app description")
    version = d[32 + 16: 32 + 48].split(b"\0")[0].decode(errors="replace")
    project = d[32 + 48: 32 + 80].split(b"\0")[0].decode(errors="replace")
    return project, version


def desc_build(d):
    """The build number: the app description's date ("Mmm dd yyyy")
    and time ("hh:mm:ss") read as UTC; None when not in that form."""
    t = d[32 + 80: 32 + 96].split(b"\0")[0].decode(errors="replace")
    day = d[32 + 96: 32 + 112].split(b"\0")[0].decode(errors="replace")
    if not re.fullmatch(r"[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4}", day) or not re.fullmatch(r"[0-9]{2}:[0-9]{2}:[0-9]{2}", t):
        return None
    try:
        return calendar.timegm(time.strptime(day.replace("  ", " 0") + " " + t, "%b %d %Y %H:%M:%S"))
    except ValueError:
        return None


def stamp(d, build):
    """date and time of the app description for the build number"""
    tm = time.gmtime(build)
    months = "JanFebMarAprMayJunJulAugSepOctNovDec"
    day = "%s %2d %04d" % (months[3 * (tm.tm_mon - 1): 3 * tm.tm_mon], tm.tm_mday, tm.tm_year)
    d[32 + 80: 32 + 96] = time.strftime("%H:%M:%S", tm).encode().ljust(16, b"\0")
    d[32 + 96: 32 + 112] = day.encode().ljust(16, b"\0")
    return fix(d)


def fix(d):
    """Checksum and appended SHA-256 recomputed in place."""
    n, _, csum_at, end = layout(d)
    d[csum_at] = checksum(d, n)
    if d[23] == 1:
        d[csum_at + 1: csum_at + 33] = hashlib.sha256(bytes(d[: csum_at + 1])).digest()
    return end


def server_block(d, n_end):
    at = d.find(MARKER, 0, n_end)
    if at < 0:
        return None, None, None
    host = bytes(d[at + 8: at + 72]).split(b"\0")[0].decode(errors="replace")
    port = struct.unpack_from("<H", d, at + 72)[0]
    return at, host, port


def build_blocks(d, end):
    """Offsets of build blocks (CC3200 build block layout, family CFW1)."""
    magic = struct.pack("<I", 0xBEAC0005)
    found, i = [], 0
    while True:
        i = d.find(magic, i, end)
        if i < 0 or i + 112 > end:
            return found
        if d[i + 108: i + 112] == magic and d[i + 93: i + 97] == b"CFW1":
            found.append(i)
        i += 1


def check(d):
    problems = []
    n, seg_end, csum_at, end = layout(d)
    chip = struct.unpack_from("<H", d, 12)[0]
    if chip != CHIP_ESP32S3:
        problems.append("chip ID %d, not 9 (ESP32-S3)" % chip)
    if d[2] != 2:
        problems.append("flash mode %d, not DIO" % d[2])
    if d[3] >> 4 != 4:  # 16 MB
        problems.append("flash size code %d, not 16 MB" % (d[3] >> 4))
    if d[3] & 15 != 0xF:  # 80 MHz
        problems.append("flash frequency code %d, not 80 MHz" % (d[3] & 15))
    if d[23] != 1:
        problems.append("no appended SHA-256")
    if end > SLOT:
        problems.append("%d bytes, more than the slot (%d)" % (end, SLOT))
    if d[csum_at] != checksum(d, n):
        problems.append("checksum wrong")
    if d[23] == 1 and bytes(d[csum_at + 1: csum_at + 33]) != hashlib.sha256(bytes(d[: csum_at + 1])).digest():
        problems.append("appended SHA-256 wrong")
    project, version = app_desc(d)
    if project != PROJECT_CFW:
        problems.append("project %r, not %r" % (project, PROJECT_CFW))
    if not version.startswith("CFW_"):
        problems.append("version %r without CFW_" % version)
    count = bytes(d[:end]).count(MARKER)
    if count != 1:
        problems.append("server block marker %d times" % count)
    if build_blocks(d, end):
        problems.append("a CC3200 build block in the image")
    if desc_build(d) is None:
        problems.append("build date and time of the app description empty or not in the form of __DATE__ __TIME__")
    return problems


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    d = bytearray(open(path, "rb").read())
    if cmd == "check":
        try:
            p = check(d)
        except ImageError as e:
            p = [str(e)]
        for x in p:
            print("FAIL:", x)
        if not p:
            project, version = app_desc(d)
            print("ok: %s %s build %d, %d bytes" % (project, version, desc_build(d), layout(d)[3]))
        return 1 if p else 0
    if cmd == "stamp":
        header = open(argv[3]).read()
        m = re.search(r"#define CFW_BUILD_TIME ([0-9]+)u", header)
        if not m:
            print("FAIL: no CFW_BUILD_TIME in %s" % argv[3])
            return 1
        build = int(m.group(1))
        if desc_build(d) != build:
            end = stamp(d, build)
            open(path, "r+b").write(bytes(d[:end]))
        return 0
    if cmd == "info":
        end = layout(d)[3]
        project, version = app_desc(d)
        at, host, port = server_block(d, end)
        print("project %s, version %s, build %s, image %d bytes, file %d bytes" % (project, version, desc_build(d), end, len(d)))
        print("server block: %s" % ("none" if at is None else "%s port %d" % (host or "(empty)", port)))
        if len(d) == end + 64:
            ok = d[end:].decode() == hashlib.sha256(bytes(d[:end])).hexdigest()
            print("digest text: %s" % ("valid" if ok else "WRONG"))
        return 0
    if cmd == "body":
        out = argv[3]
        host, port = None, 0
        rest = argv[4:]
        while rest:
            if rest[0] == "--host":
                host = rest[1]
            elif rest[0] == "--port":
                port = int(rest[1])
            rest = rest[2:]
        end = layout(d)[3]
        d = d[:end]
        if host is not None:
            at, _, _ = server_block(d, end)
            if at is None:
                print("FAIL: no server block")
                return 1
            h = host.encode()
            if len(h) > 64 or not 0 <= port <= 65535:
                print("FAIL: host longer than 64 bytes or port out of range")
                return 1
            d[at + 8: at + 72] = h.ljust(64, b"\0")
            struct.pack_into("<H", d, at + 72, port)
            d[at + 74: at + 80] = bytes(6)
        end = fix(d)
        body = bytes(d[:end]) + hashlib.sha256(bytes(d[:end])).hexdigest().encode()
        open(out, "wb").write(body)
        print("%s: %d bytes" % (out, len(body)))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
