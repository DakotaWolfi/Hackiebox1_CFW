#!/bin/sh
# Host test of esp32/main/assets.c with the shared FatFs:
# the copies in /CFW/ on a FAT image made by ESP-IDF's fatfsgen.py, read
# back afterwards by its fatfsparse.py; the wear-levelling check with
# images of wl_fatfsgen.py (IDF_PYTHON: a python with "construct").
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/../../.."
T=esp32/tests/assets; B=build/host/assets
IDF=${IDF_PATH:-$HOME/esp/esp-idf-v5.4.2}
PY=${IDF_PYTHON:-$HOME/.espressif/python_env/idf5.4_py3.13_env/bin/python}
der() { # a DER element of $2 bytes in all into $1
    python3 -c "import os,sys; n=$2-4; open(sys.argv[1],'wb').write(bytes([0x30,0x82,n>>8,n&255])+os.urandom(n))" "$1"
}
rm -rf $B && mkdir -p $B/in/CERT $B/in/FW $B/in/WWW
der $B/in/CERT/CA.DER 913
der $B/in/CERT/CLIENT.DER 601
der $B/in/CERT/PRIVATE.DER 1192
der $B/new.der 777
head -c 70000 /dev/urandom > $B/in/FW/SFX.BIN
head -c 5000 /dev/urandom > $B/in/WWW/INDEX.HTM
# the data area of the 1408 KB partition: 348 sectors
$PY $IDF/components/fatfs/fatfsgen.py --output_file $B/fat.img --partition_size 1425408 --sector_size 4096 --fat_type 12 $B/in > /dev/null
$PY $IDF/components/fatfs/wl_fatfsgen.py --output_file $B/wl.img --partition_size 1441792 --sector_size 4096 --fat_type 12 $B/in > /dev/null
gcc -w -g -O1 -DCFW_ESP32=1 -Isrc -Ithird_party/fatfs -c -o $B/ff.o src/fatfs/ff.c
gcc -fsanitize=address,undefined -g -O1 -Wall -Wextra -Wno-unused-parameter -DCFW_ESP32=1 -I$T -Isrc -Ithird_party/fatfs -o $B/test \
    $T/test.c esp32/main/assets.c src/diskio.c $B/ff.o
echo "== wear levelling state"
$B/test gate $B/wl.img 1
for k in copy1 both v1 maxpos; do
    $PY $T/state.py $B/wl.img $B/wl-$k.img $k
    case $k in copy1) want=1 ;; *) want=0 ;; esac
    $B/test gate $B/wl-$k.img $want
done
echo "== files"
$B/test files $B/fat.img $B/in $B/new.der
echo "== the image read by ESP-IDF's fatfsparse.py"
rm -rf $B/out && mkdir $B/out && (cd $B/out && $PY $IDF/components/fatfs/fatfsparse.py ../fat.img > /dev/null)
ok=0
for f in CERT/CA.DER CERT/CLIENT.DER CERT/PRIVATE.DER FW/SFX.BIN WWW/INDEX.HTM; do
    cmp -s $B/in/$f $B/out/Espressif/$f && echo "  ok: /$f unchanged" || { echo "  FAIL: /$f"; ok=1; }
done
cmp -s $B/new.der $B/out/Espressif/CFW/CLIENT.DER && echo "  ok: /CFW/CLIENT.DER the uploaded file" || { echo "  FAIL: /CFW/CLIENT.DER"; ok=1; }
cmp -s $B/in/CERT/CA.DER $B/out/Espressif/CFW/CA.DER && echo "  ok: /CFW/CA.DER the copy" || { echo "  FAIL: /CFW/CA.DER"; ok=1; }
exit $ok
