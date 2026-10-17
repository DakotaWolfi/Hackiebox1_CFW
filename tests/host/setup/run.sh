#!/bin/sh
# Host test of setup.c (setup mode data rules). usage: tests/host/setup/run.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
B=build/host; mkdir -p $B
gcc -fsanitize=address,undefined -g -O1 -Wall -Wextra -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/setup_test tests/host/setup/setup_test.c src/setup.c
$B/setup_test
