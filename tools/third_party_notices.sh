#!/bin/bash
# The licence texts of the third-party code in the release images, as the
# licences ask for when the code is passed on in binary form.
#
# usage: tools/third_party_notices.sh IDF_PATH > THIRD-PARTY-NOTICES.txt
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/.."
IDF=$1
section() {
    printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1"
}
cat <<'HEAD'
Third-party software in the Toniebox custom firmware
====================================================

The firmware itself is GPL-3.0-or-later. The images contain the following
third-party code under its own licence; the texts follow below.

CC3200 image (cc3200/ng-cfw.bin) and ESP32 image (esp32/toniebox-cfw-esp32.bin):
  - Opus 1.1 audio codec (BSD-3-Clause)
  - FatFs R0.15a by ChaN (FatFs licence, BSD-1-Clause style)
CC3200 image only:
  - FreeRTOS kernel (MIT)
  - TI SimpleLink CC3200 host driver, Texas Instruments (BSD-3-Clause)
  - TI CC3200 SDK driver library, linked from the SDK (BSD-3-Clause)
ESP32 image only:
  - ESP-IDF 5.4.2 by Espressif Systems (Apache-2.0), including the FreeRTOS
    kernel (MIT), lwIP (BSD-3-Clause), Mbed TLS (Apache-2.0) and
    wpa_supplicant (BSD-3-Clause); the full list of ESP-IDF's components and
    their licences is in ESP-IDF's COPYRIGHT.rst, included at the end.

The SD bootloader (cc3200/ngbootloader.bin, cc3200/flash/sys/mcuimg.bin) is
built from HackieboxNG by SciLor and contributors with the changes in the
hackiebox_cfw_ng_cfw repository; the upstream project publishes no licence
file.
HEAD
section "Opus 1.1 (third_party/opus-1.1/COPYING)"
cat third_party/opus-1.1/COPYING
section "FatFs (third_party/fatfs/LICENSE.txt)"
cat third_party/fatfs/LICENSE.txt
section "FreeRTOS kernel (third_party/freertos/LICENSE.md)"
cat third_party/freertos/LICENSE.md
section "TI SimpleLink host driver and CC3200 SDK driver library"
sed -n '4,33p' third_party/simplelink/source/device.c | sed 's/^ \?\* \?//'
if [ -n "$IDF" ] && [ -f "$IDF/LICENSE" ]; then
    section "ESP-IDF (LICENSE)"
    cat "$IDF/LICENSE"
    section "lwIP (components/lwip/lwip/COPYING)"
    cat "$IDF/components/lwip/lwip/COPYING"
    section "wpa_supplicant (components/wpa_supplicant/COPYING)"
    cat "$IDF/components/wpa_supplicant/COPYING"
    section "FreeRTOS kernel of ESP-IDF (components/freertos/FreeRTOS-Kernel/LICENSE.md)"
    cat "$IDF/components/freertos/FreeRTOS-Kernel/LICENSE.md"
    section "Mbed TLS: Apache-2.0, as ESP-IDF above"
    section "ESP-IDF third-party components (docs/en/COPYRIGHT.rst)"
    cat "$IDF/docs/en/COPYRIGHT.rst"
fi
