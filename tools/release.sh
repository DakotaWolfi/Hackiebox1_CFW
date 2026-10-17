#!/bin/bash
# Release package: both images, the setup page and the
# README in one zip whose files are stored without compression, so that
# teddyCloud reads it without a decompressor and each file can be taken
# out on its own.
#
# usage: tools/release.sh VERSION      e.g. tools/release.sh 0.2.0
#   -> build/release/toniebox-cfw-VERSION.zip
# Needs a clean tree (the build number is the commit time) and
# ESP-IDF in ~/esp/esp-idf-v5.4.2 (or IDF_PATH).
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
OUT=build/release
PKG=$OUT/pkg
make clean >/dev/null
mkdir -p "$PKG/cc3200" "$PKG/esp32"

echo "== CC3200"
CC_BUILD=$(make -j"$(nproc)" CFW_VERSION="$V" | sed -n 's/^build\/cfw-slot.bin: .*build \([0-9]*\)$/\1/p')
cp build/cfw-slot.bin "$PKG/cc3200/ng-cfw.bin"

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
        -e "s/@ESP32_BUILD@/$ESP_BUILD/g" -e "s/@PAGE_VERSION@/$PAGE_VERSION/g" -e "s/@PAGE_NEEDS@/$PAGE_NEEDS/g" "$t" > "$PKG/$(basename "$t" .in)"
done
(cd "$PKG" && sha256sum cc3200/ng-cfw.bin esp32/toniebox-cfw-esp32.bin setup.htm > SHA256SUMS)

ZIP=$OUT/toniebox-cfw-$V.zip
(cd "$PKG" && zip -q -0 -X -r "../$(basename "$ZIP")" README.md SHA256SUMS cc3200 esp32 setup.htm)
echo "== $ZIP (CFW_$V, $REV; CC3200 build $CC_BUILD, ESP32 build $ESP_BUILD; setup page $PAGE_VERSION, needs $PAGE_NEEDS)"
unzip -v "$ZIP" | sed -n '4,20p'
