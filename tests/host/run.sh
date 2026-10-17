#!/bin/sh
# Host tests: player.c, cloud.c and rtnl.c built for the PC with stubs for
# FreeRTOS, FatFs, SimpleLink and the other modules, run with the address
# and undefined-behaviour sanitizers.
# usage: tests/host/run.sh <content file with three chapters, e.g. from tools/taftest>
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../.."
T=tests/host; B=build/host; O=third_party/opus-1.1; TAF=$(readlink -f "$1")
mkdir -p $B/card/CONTENT/B706EE0E && cp "$TAF" $B/card/CONTENT/B706EE0E/500304E0
export ASAN_OPTIONS=detect_leaks=0
SAN="-fsanitize=address,undefined -g -O1 -w -Uunix -DCFW_DEBUG=2"
DEF="-DCFW_BUILD_TIME=1791775993u -DCFW_REVISION=\"test\" -DCFW_VERSION=\"0.1.0\" -DCFW_BUILD_DATE=\"x\""
gcc $SAN $DEF -DFIXED_POINT -DDISABLE_FLOAT_API -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -I$T/player/inc -Itests/host/inc -Isrc -Isrc/cc3200 -I$O/include -I$O/celt -I$O/silk -I$O \
  -o $B/player_test $T/player/harness.c src/player.c src/sfx.c src/xplay.c src/ext.c src/crc32.c src/taf.c $O/celt/bands.c $O/celt/celt.c $O/celt/celt_decoder.c $O/celt/cwrs.c \
  $O/celt/entcode.c $O/celt/entdec.c $O/celt/entenc.c $O/celt/kiss_fft.c $O/celt/laplace.c $O/celt/mathops.c $O/celt/mdct.c \
  $O/celt/modes.c $O/celt/pitch.c $O/celt/celt_lpc.c $O/celt/quant_bands.c $O/celt/rate.c $O/celt/vq.c $O/src/opus.c \
  $O/src/opus_decoder.c third_party/opus_glue/silk_stub.c -lm
echo "== player: play, chapters, wind, removal, resume"
$B/player_test $B/card "0S,0P,5000F,10000F,15000B,17000B,40000W,45000V,50000R,0P,60000B" | grep -v "end-of-stream\|box activity\|SOUND limits\|LED level"
echo "== player: prompt during content, removal held by an online round"
$B/player_test $B/card "0S,0P,3000X,8000R" | grep -v "end-of-stream\|box activity"
echo "== player: critical battery during content: no restart of the content after prompt 9"
rm -rf $B/pcard && mkdir -p $B/pcard/CONTENT/B706EE0E $B/pcard/CONTENT/00000000 && cp "$TAF" $B/pcard/CONTENT/B706EE0E/500304E0 && cp "$TAF" $B/pcard/CONTENT/00000000/00000009
$B/player_test $B/pcard "0S,0P,3000C,20000R" > $B/critical.log
grep "^---\|sound prompt\|pl: play\|pl: prompt at" $B/critical.log
[ "$(grep -c "pl: play" $B/critical.log)" = 1 ] || { echo "FAIL: the content started again after prompt 9"; exit 1; }
HOLD=2000:6000 $B/player_test $B/card "0S,0P,3000R" | grep -v "end-of-stream\|box activity" | tail -3
echo "== player: download of a missing content completes: chime 7 for every audio_id"
D=$B/dcard; rm -rf $D && mkdir -p $D/CONTENT/B706EE0E
CLOUD=dl:$TAF $B/player_test $D "0S,0P,30000R" > $B/download.log
grep "download done\|download complete\|CHIME 7" $B/download.log
grep -q "CHIME 7" $B/download.log || { echo "FAIL: no chime 7 after the download"; exit 1; }
# teddyCloud extension: a fresh card with two figures; F the figure, E the event tag
F=B706EE0E500304E0; E=11223344500304E0; X=$B/xcard
xcard() { rm -rf $X && mkdir -p $X/CONTENT/B706EE0E $X/CONTENT/C1C2C3C4 && cp "$TAF" $X/CONTENT/B706EE0E/500304E0 && cp "$TAF" $X/CONTENT/C1C2C3C4/500304E0; }
KEY="ext:\|stored position\|shuffle\|SOUND path\|SOUND fade\|SOUND ear\|feedback\|CHIME\|restart\|RECORD\|^---"
echo "== EXT F1 resume per tag (another figure in between)"
xcard; python3 $T/player/tbs.py $X profile $F resume=1
$B/player_test $X "0S,0P,5000R,0K,8000R,0P,12000R" | grep "$KEY\|start position"; python3 $T/player/tbs.py $X showstate
echo "== EXT F2 shuffle and F6 speaker limit 4"
xcard; python3 $T/player/tbs.py $X profile $F shuffle=1 spk=4
$B/player_test $X "0S,0P,3000R,0P,6000F,8000B,80000R" | grep "$KEY\|limits 4"; python3 $T/player/tbs.py $X showstate
echo "== EXT F4 event tag for the next figure (resume, shuffle on, sleep 20, slap lock), twice"
xcard; python3 $T/player/tbs.py $X event $E 1:0:0 2:1:0 3:0:20 10:0:0
for i in 1 2; do $B/player_test $X "0S,0Q,1000R,0P,3000R" | grep "written\|feedback"; python3 $T/player/tbs.py $X show $F; done
echo "== EXT F11 global tag: slap lock, volume (start 6) and LED level with the ears, combined"
xcard; python3 $T/player/tbs.py $X event $E 13:0:0 9:0:6 8:0:0
$B/player_test $X "0S,0Q,0U,0U,0U,0H,0L,0L,0H,0R" | grep "$KEY"; python3 $T/player/tbs.py $X show global
echo "== EXT F4 values set with the ears for the next figure (5, 6), F5 restart gesture"
xcard; python3 $T/player/tbs.py $X event $E 5:0:12 6:0:2
$B/player_test $X "0S,0Q,0U,0U,0H,0L,0R,0P,2000T,5000R" | grep "$KEY"; python3 $T/player/tbs.py $X show $F
echo "== EXT F3 sleep timer 1 min: ear step in the fade, removal and replacement, power-off"
xcard; python3 $T/player/tbs.py $X profile $F sleep=1
IDLETIME=1 FADELOG=1 $B/player_test $X "0S,0P,10000U,30000U,35000G,40000R,50000P,90000R" | grep "$KEY\|sleep\|power-off\|kept" | awk 'NR % 4 == 1 || !/fade/'
echo "== EXT F3 time up during playback: stop as on removal, power-off"
IDLETIME=1 $B/player_test $X "0S,0P,50000R,60000P" | grep "sleep\|power-off\|EVENT 1[23]\|stopped"
echo "== EXT F9 both ears during playback with a sleep timer: stop, timer ends, power-off; global 0x07 = 0: nothing"
xcard; python3 $T/player/tbs.py $X profile $F sleep=1
IDLETIME=1 $B/player_test $X "0S,0P,5000O,6000R,7000P" | grep "$KEY\|sleep\|power-off\|both\|EVENT\|^--- place"
python3 $T/player/tbs.py $X global ears=0
$B/player_test $X "0S,0P,5000O,8000R" | grep "power-off\|both\|EVENT\|^---"
echo "== EXT F12 player API: status, pause, chapter in the pause, continue, volume, stop, remote play"
xcard; python3 $T/player/tbs.py $X profile C1C2C3C4500304E0 spk=10
IDLETIME=1 $B/player_test $X "0S,0P,2000i,3000p,4000i,5000n,7000p,8000c,9000v,10000s,11000i,12000r,13000R,14000r,16000i,18000P,20000i" | grep "status\|remote\|pause\|^--- place\|EVENT 1[13]"
rm -rf $B/ccard && mkdir -p $B/ccard/CONTENT/B706EE0E && cp "$TAF" $B/ccard/CONTENT/B706EE0E/500304E0
gcc $SAN $DEF -I$T/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/cloud_test $T/cloud/harness.c src/cc3200/flashfs.c src/sdfiles.c src/taf.c src/clock.c
echo "== cloud"
# the exit code counts: a pipe into grep alone would hide failures
st=0; $B/cloud_test $B/ccard "$TAF" > $B/cloud.log || st=$?
grep "^T\|ok:\|FAIL\|failure" $B/cloud.log
[ $st = 0 ] || { echo "FAIL: cloud test ($st)"; exit 1; }
rm -rf $B/ecard && mkdir -p $B/ecard/CONTENT/B706EE0E && cp "$TAF" $B/ecard/CONTENT/B706EE0E/500304E0
gcc $SAN $DEF -DCFW_ESP32=1 -I$T/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/cloud_esp32_test $T/cloud/harness.c src/cc3200/flashfs.c src/sdfiles.c src/taf.c src/clock.c
echo "== cloud, ESP32 box"
st=0; $B/cloud_esp32_test $B/ecard "$TAF" > $B/cloud_esp32.log || st=$?
grep "^E\|ok:\|FAIL\|failure" $B/cloud_esp32.log
[ $st = 0 ] || { echo "FAIL: cloud test ESP32 ($st)"; exit 1; }
gcc $SAN $DEF -I$T/rtnl -I$T/cloud/inc -Itests/host/inc -Isrc -Isrc/cc3200 -o $B/rtnl_test $T/rtnl/harness.c
echo "== rtnl stream (frames)"
$B/rtnl_test | od -An -tx1 | head -4
