#!/bin/sh
# Host test of the battery rules and the battery report (power.c).
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
B=build/host/battery
mkdir -p $B
gcc -fsanitize=address,undefined -g -O1 -Wall -Wno-unused-function -Uunix -Itests/host/player/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/battery_test tests/host/battery/harness.c
$B/battery_test
