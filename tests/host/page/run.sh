#!/bin/sh
# Host test of the setup page server (page.c) with socket,
# card, net and cloud stubs. usage: tests/host/page/run.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
B=build/host/page
rm -rf $B && mkdir -p $B/card
gcc -fsanitize=address,undefined -g -O1 -Wall -Wno-unused-function -Uunix -Itests/host/page/inc -Itests/host/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/page_test tests/host/page/harness.c src/setup.c
$B/page_test $B/card
