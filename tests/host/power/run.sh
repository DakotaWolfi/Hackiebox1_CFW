#!/bin/sh
# Host test of the retained state encoding and the card journal.
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
B=build/host/power
rm -rf $B && mkdir -p $B/card
gcc -fsanitize=address,undefined -g -O1 -Wall -Wno-unused-function -Uunix -Itests/host/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/power_test tests/host/power/harness.c src/cc3200/flashfs.c src/sdfiles.c src/taf.c src/clock.c
$B/power_test $B/card
