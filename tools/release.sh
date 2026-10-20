#!/bin/bash
# Release package: both images, the SD bootloader of the CC3200 box with its
# preloader and a ready SD card tree, the setup page and the README in one zip whose files are stored without compression, so that
# teddyCloud reads it without a decompressor and each file can be taken
# out on its own.
#
# usage: tools/release.sh VERSION      e.g. tools/release.sh 0.2.0
#   -> build/release/toniebox-cfw-VERSION.zip
# Needs a clean tree (the build number is the commit time), ESP-IDF in
# ~/esp/esp-idf-v5.4.2 (or IDF_PATH) and a clean checkout of the SD
# bootloader (hackiebox_cfw_ng_cfw beside this repository, or BOOTLOADER).
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/.."
V=$1
case "$V" in
[0-9]*.[0-9]*.[0-9]*) ;;
*) echo "usage: $0 MAJOR.MINOR.PATCH" >&2; exit 2 ;;
esac
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "release: the tree has changes; commit first" >&2
    exit 1
fi
REV=$(git rev-parse --short=7 HEAD)
BL=${BOOTLOADER:-../hackiebox_cfw_ng_cfw}
if [ ! -x "$BL/common/buildAndShip.bootloader.sh" ]; then
    echo "release: no SD bootloader checkout at $BL (set BOOTLOADER)" >&2
    exit 1
fi
if [ -n "$(git -C "$BL" status --porcelain --untracked-files=no)" ]; then
    echo "release: the SD bootloader checkout has changes; commit first" >&2
    exit 1
fi
BL_REV=$(git -C "$BL" rev-parse --short=7 HEAD)
OUT=build/release
PKG=$OUT/pkg
make clean >/dev/null
mkdir -p "$PKG/cc3200" "$PKG/esp32"

echo "== CC3200"
CC_BUILD=$(make -j"$(nproc)" CFW_VERSION="$V" | sed -n 's/^build\/cfw-slot.bin: .*build \([0-9]*\)$/\1/p')
cp build/cfw-slot.bin "$PKG/cc3200/ng-cfw.bin"

echo "== SD bootloader"
"$BL/common/buildAndShip.bootloader.sh" >/dev/null
BL_OUT="$BL/exe/sd-bootloader-ng"
cp "$BL_OUT/sd/revvox/boot/ngbootloader.bin" "$PKG/cc3200/ngbootloader.bin"
mkdir -p "$PKG/cc3200/flash/sys" "$PKG/cc3200/card/cfw"
cp "$BL_OUT/flash/sys/mcuimg.bin" "$PKG/cc3200/flash/sys/mcuimg.bin"
# the card for a first installation: the bootloader and its settings, this firmware in slot cfw1 started on its
# own, and the setup page; the original firmware's slot (ng-ofw1.bin) comes from the box
cp -r "$BL_OUT/sd/revvox" "$PKG/cc3200/card/"
# the bootloader's patches change the original firmware; they are not part of this package
rm -rf "$PKG/cc3200/card/revvox/boot/patch"
cp "$PKG/cc3200/ng-cfw.bin" "$PKG/cc3200/card/revvox/boot/ng-cfw1.bin"
cp card/cfw/setup.htm "$PKG/cc3200/card/cfw/setup.htm"
# the boot state file the bootloader keeps its slots in; without it no update through teddyCloud
python3 tools/bootstate.py write --active cfw1 --next none --kind none --tries 0 --helper none \
    "$PKG/cc3200/card/revvox/boot/ngstate.bin" >/dev/null
python3 - "$PKG/cc3200/card/revvox/boot/ngCfg.json" <<'PY'
import json, sys
path = sys.argv[1]
cfg = json.load(open(path))
general = {}
for key, value in cfg["general"].items():
    general[key] = value
    if key == "activeImg":
        general["activeImg"] = "cfw1"
        general["stateFile"] = "/revvox/boot/ngstate.bin"
cfg["general"] = general
# the images carry their SHA-256 digest at the end: the bootloader never starts a damaged one
for slot in ("cfw1", "cfw2", "cfw3"):
    cfg.setdefault(slot, {})["checkHash"] = True
open(path, "w").write(json.dumps(cfg, indent=4) + "\n")
PY

echo "== ESP32"
IDF=${IDF_PATH:-$HOME/esp/esp-idf-v5.4.2}
(. "$IDF/export.sh" >/dev/null 2>&1 && cd esp32 && idf.py -DCFW_VERSION="$V" build >/dev/null)
cp esp32/build/toniebox-cfw-esp32.bin "$PKG/esp32/toniebox-cfw-esp32.bin"
ESP_BUILD=$(python3 tools/esp32img.py info esp32/build/toniebox-cfw-esp32.bin | sed -n 's/.*build \([0-9]*\).*/\1/p')

# the page as it is: its own version and the firmware it needs at least; a page whose
# content changes gets a new cfw-page-version in the same commit
PAGE_VERSION=$(sed -n 's/.*name="cfw-page-version" content="\([^"]*\)".*/\1/p' card/cfw/setup.htm)
PAGE_NEEDS=$(sed -n 's/.*name="cfw-page-requires" content="\([^"]*\)".*/\1/p' card/cfw/setup.htm)
[ -n "$PAGE_VERSION" ] && [ -n "$PAGE_NEEDS" ] || { echo "release: setup.htm needs cfw-page-version and cfw-page-requires" >&2; exit 1; }
printf '%s\n%s\n' "$PAGE_NEEDS" "CFW_$V" | sort -C -V || { echo "release: the page needs $PAGE_NEEDS, newer than CFW_$V" >&2; exit 1; }
cp card/cfw/setup.htm "$PKG/setup.htm"
for t in release/README*.md.in; do
    sed -e "s/@VERSION@/CFW_$V/g" -e "s/@REVISION@/$REV/g" -e "s/@CC3200_BUILD@/$CC_BUILD/g" \
        -e "s/@ESP32_BUILD@/$ESP_BUILD/g" -e "s/@BOOTLOADER_REVISION@/$BL_REV/g" -e "s/@PAGE_VERSION@/$PAGE_VERSION/g" -e "s/@PAGE_NEEDS@/$PAGE_NEEDS/g" "$t" > "$PKG/$(basename "$t" .in)"
done
tools/third_party_notices.sh "$IDF" > "$PKG/THIRD-PARTY-NOTICES.txt"
(cd "$PKG" && sha256sum cc3200/ng-cfw.bin cc3200/ngbootloader.bin cc3200/flash/sys/mcuimg.bin esp32/toniebox-cfw-esp32.bin setup.htm > SHA256SUMS)

ZIP=$OUT/toniebox-cfw-$V.zip
(cd "$PKG" && zip -q -0 -X -r "../$(basename "$ZIP")" README.md THIRD-PARTY-NOTICES.txt SHA256SUMS cc3200 esp32 setup.htm)
echo "== $ZIP (CFW_$V, $REV; CC3200 build $CC_BUILD, SD bootloader $BL_REV, ESP32 build $ESP_BUILD; setup page $PAGE_VERSION, needs $PAGE_NEEDS)"
unzip -v "$ZIP" | sed -n '4,20p'
