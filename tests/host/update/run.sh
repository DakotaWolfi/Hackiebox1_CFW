#!/bin/sh
# Host test of the item-3 installation (update.c, sha256.c, bootstate.c):
# images made with tools/mkimage.py, FatFs on a card directory.
# usage: tests/host/update/run.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
T=tests/host/update; B=build/host/update
rm -rf $B && mkdir -p $B/img $B/card/revvox/boot
RUNNING=1791775993
python3 - $B/img $RUNNING <<'PY'
import struct, sys, subprocess, os
out, running = sys.argv[1], int(sys.argv[2])
def raw(name, build, marker=b"CFW1", kind=2, debug=0, size=40000):
    body = bytes((i * 7) & 255 for i in range(size))
    bb = struct.pack("<IIII", 0xBEAC0005, 0, build, debug) + b"abcdef0\0" + b"01 Jan 00:00".ljust(61, b"\0")
    bb += bytes([1, 2, 0, 1, 0, 1, 14, 0]) + marker + bytes(3) + struct.pack("<III", kind, 0, 0xBEAC0005)
    assert len(bb) == 112
    p = os.path.join(out, name + ".raw")
    open(p, "wb").write(body + bb)
    return p
def slot(name, *a, **k):
    p = raw(name, *a, **k)
    if subprocess.call(["python3", "tools/mkimage.py", p, os.path.join(out, name + ".bin")], stdout=subprocess.DEVNULL):
        # mkimage refuses some on purpose: append the digest here
        import hashlib
        d = open(p, "rb").read()
        open(os.path.join(out, name + ".bin"), "wb").write(d + hashlib.sha256(d).hexdigest().encode())
slot("good", running - 86400)          # older: accepted
slot("same_build", running)
slot("no_marker", running + 1, marker=b"\0\0\0\0")
slot("kind3", running + 1, kind=3)
slot("debug", running + 1, debug=1)
d = bytearray(open(os.path.join(out, "good.bin"), "rb").read())
d[-1] ^= 1 if chr(d[-1]) != "0" else 2
open(os.path.join(out, "bad_digest.bin"), "wb").write(d)
PY
python3 tools/bootstate.py write $B/card/revvox/boot/ngstate.bin --active none
printf '{\n  "general": {\n    "activeImg": "cfw1",\n    "stateFile" : "/revvox/boot/ngstate.bin"\n  }\n}\n' > "$B/card/revvox/boot/ngCfg.json"
# what FatFs without long names sees of it
cp $B/card/revvox/boot/ngCfg.json "$B/card/revvox/boot/NGCFG~1.JSO"
# cfw1 (the running slot) holds an image with the marker
cp $B/img/good.bin $B/card/revvox/boot/ng-cfw1.bin
gcc -fsanitize=address,undefined -g -O1 -Wall -Wno-unused-function -DCFW_BUILD_TIME=${RUNNING}u -DCFW_VERSION=\"0.1.0\" \
  -I$T/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/update_test $T/harness.c
$B/update_test $B/card $B/img
python3 tools/bootstate.py show $B/card/revvox/boot/ngstate.bin
