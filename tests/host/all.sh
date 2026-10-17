#!/bin/sh
# All host tests: player, cloud and log stream (run.sh), log connection,
# settings file, setup-mode rules, update install, retained state and the
# setup page server.
# usage: tests/host/all.sh <content file with three chapters, e.g. from tools/taftest>
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../.."
sh tests/host/run.sh "$1"
echo "== settings file"
gcc -fsanitize=address,undefined -g -O1 -w -Itests/host/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o build/host/settings_test tests/host/cloud/settings_test.c src/cc3200/flashfs.c
ASAN_OPTIONS=detect_leaks=0 build/host/settings_test > build/host/settings.log || { cat build/host/settings.log; exit 1; }
grep "failure" build/host/settings.log || true
for t in rtnl setup update power battery page; do
    echo "== $t"
    sh tests/host/$t/run.sh > build/host/$t.log 2>&1 || { cat build/host/$t.log; echo "$t FAILED"; exit 1; }
    tail -1 build/host/$t.log
done
