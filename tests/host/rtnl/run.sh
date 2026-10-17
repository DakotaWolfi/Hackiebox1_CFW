#!/bin/sh
# Host tests of rtnl.c: the encoder (harness.c, frames to stdout) and the
# log connection (conn_test.c), with the address and undefined-behaviour
# sanitizers. usage: tests/host/rtnl/run.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
T=tests/host/rtnl; B=build/host; mkdir -p $B
SAN="-fsanitize=address,undefined -g -O1 -Uunix -Wall -Wno-unused-function"
DEF="-DCFW_BUILD_TIME=1791775993u -DCFW_REVISION=\"test\" -DCFW_VERSION=\"0.1.0\" -DCFW_BUILD_DATE=\"x\""
gcc $SAN $DEF -I$T -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/rtnl_conn_test $T/conn_test.c
$B/rtnl_conn_test
gcc $SAN $DEF -I$T -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/rtnl_test $T/harness.c
echo "== rtnl stream (frames)"
$B/rtnl_test | od -An -tx1 | head -4
