# Custom firmware for the ESP32-S3 box

The custom firmware for the Toniebox with the ESP32-S3 (board
`TONIEBOX-ESP32` rev 1.6.C). It is the same firmware as the
CC3200 one: every module in `../src/` is compiled here unchanged, this
directory adds only the platform part (drivers, Wi-Fi, TLS, flash, slots).

Licence: GPL-3.0-or-later (`COPYING`), like the whole repository; ESP-IDF
is Apache-2.0, which GPL-3.0 allows to combine with.

**Status:** builds, host tests of the shared code and of the assets partition code
pass. Nothing has run on a box yet.

## Build

ESP-IDF 5.4.2 for the ESP32-S3 (`install.sh esp32s3`), then:

```sh
. ~/esp/esp-idf-v5.4.2/export.sh
cd esp32
idf.py build                                  # build/toniebox-cfw-esp32.bin
python3 ../tools/esp32img.py check build/toniebox-cfw-esp32.bin
```

`CFW_VERSION`, `CFW_DEBUG` and `CFW_START_STEP` are CMake cache variables
(`idf.py -DCFW_VERSION=0.2.0 build`). The build number is the commit time,
as for the CC3200 (a tree with uncommitted changes builds with the current
time). After a change of `sdkconfig.defaults`, delete `sdkconfig` so that it
is made again.

The image is an app image for the box's own bootloader (ESP-IDF 5.0) and
partition table at 0x9000 (`partitions.csv` describes it, it is never
written): DIO, 80 MHz, 8 MB, unsigned, SHA-256 appended. It
holds a server block and no build block: the build number is
the build date and time of the app description, read as UTC.
A post-build step (`esp32img.py stamp`) writes the build number of
`cfw_build.h` into those fields, so that they say the commit time like the
CC3200 build number.

## Installation through teddyCloud

On a box whose original firmware already talks to teddyCloud
(TC-patched box):

1. Box Update tab: upload `build/toniebox-cfw-esp32.bin` (the plain image or
   a body with digest text).
2. Assign the image to the box (the box shows "Original firmware" and the
   platform ESP32).
3. At its next update check the original firmware fetches `/v1/ota/3`
   (when it runs the check is measurement point M-039; a restart of the box
   is the safe way to trigger it). teddyCloud answers once with the
   image, its server block holding the host the box asked and the API port,
   writes nothing else, and the box restarts into the custom firmware.
4. First start: the firmware copies the TLS files to `/CFW/` of the assets
   partition, takes the Wi-Fi profiles of the original
   firmware, writes host and port from the server block to
   `/cfw/config.txt` and the setup page to `/cfw/setup.htm` on the card, and
   counts as a trial start until card, codec, tag reader and the first
   freshness check are fine. Three unconfirmed
   starts, a failed check or 300 s joined without an answered freshness
   check return it to the original firmware.

Without a server block the box confirms as soon as it has joined a network
and makes no cloud connection until the server is set on the setup page or
on the card.

`tools/esp32img.py body IMAGE OUT --host H --port P` makes the same body by
hand, for a test server without the extension.

## Return to the original firmware through teddyCloud

Upload the original firmware in the Box Update tab: an app image of
project `toniebox-esp32-eu` or a full flash dump of the box (the slot with
the highest version is taken). teddyCloud can apply its patches on the
way, as its ESP32 flash tool does: the host instead of the vendor hosts
(at most 12 characters), or of an old host, and the port. Assigned to the
box running this firmware, it is delivered as item 3; the firmware writes
it as delivered into the slot that is neither running nor original
(never over the original slot, which stays as a way back over the serial
port), records that slot as the original slot and restarts into it. When
the box then asks as the original firmware, teddyCloud removes the
assignment.

## Development: flashing over the serial port

The board has no USB; UART0 on J103 (TxD RxD GND, 921600 Bd like the box's bootloader; the ROM
prints its first lines at 115200) is console and
flash channel, DTR/RTS are not wired. Download mode: short J100 and power on
cold. Write the image into a free app slot and select it, never the
bootloader, the partition table, `nvs`, `assets` or `phy_init`:

```sh
esptool.py --chip esp32s3 --before no-reset --after no-reset -b 460800 \
    write_flash 0x3a0000 build/toniebox-cfw-esp32.bin      # ota_1
```

and set the boot selection with `otatool.py` from ESP-IDF
(`switch_ota_partition --slot 1`, partition table offset 0x9000). Keep a
full flash dump of the box before the first test; it is the
way back when nothing else works.

## What lives where

| Place | Content |
| --- | --- |
| assets partition `/CFW/` | `CA.DER`, `CLIENT.DER`, `PRIVATE.DER`: the firmware's TLS files, copied from `/CERT/` when missing, replaced by an upload on the setup page |
| assets partition `/CERT/` | the original firmware's TLS files: only copied from, never changed |
| NVS namespace `cfw` | `slots` (trial state), `wifi` (profiles), `lang` (box language, u8), `journal.bin` |
| assets partition `/FW/SFX.BIN` | the original firmware's sound-prompt archive, read only; unpacked to the card like the CC3200's |
| card `/cfw/` | `config.txt` (server), `setup.htm` (setup page), `ext/` (settings files of the teddyCloud extension) |
| RTC memory | the retained record of the fault handling and the unconfirmed-start count |

The original firmware's namespaces `TB_*`, everything in the assets
partition outside `/CFW/`, the bootloader and the partition table are only
read. The assets partition is handed to
ESP-IDF's wear levelling driver only after its state has been checked: the
driver formats a partition whose state it does not accept, which would
destroy the box's certificates; such a partition stays untouched and the
firmware makes no TLS connection.

## Files

| File | Role |
| --- | --- |
| `main/board.c`, `i2c.c`, `led.c`, `battery.c` | pins and supplies, I2C at 400 kHz, LED PWM, battery and charger ADC |
| `main/audio_out.c` | I2S at 16 kHz, BCLK 512 kHz for the codec's PLL |
| `main/sd.c` | the card on the 4-bit SDMMC host with the card rules of the CC3200 box |
| `main/nfc_port.c` | SPI of the TRF7962A for the shared `nfc.c` |
| `main/assets.c` | the assets partition as FatFs drive `1:` (shared `src/fatfs`), the check of the wear levelling state, the copies in `/CFW/` |
| `main/flashfs.c`, `nvsfile.c` | the flash files of the shared code |
| `main/sock.c` | sockets and TLS (mbed TLS, no SNI) |
| `main/net.c` | Wi-Fi on esp_wifi |
| `main/slots.c` | slots, trial start, rollback, item 3, return to the original firmware |
| `main/srvblock.c`, `page_default.c` | server block, built-in setup page |
| `main/sys.c`, `watchdog.c`, `debug.c`, `tick.c`, `main.c` | restart, deep sleep, retained record, watchdog, console, start-up |
| `tests/assets/` | host test of `assets.c`: the state check with images of ESP-IDF's `wl_fatfsgen.py`, the copies, uploads and power-loss cases with the shared FatFs on an image of `fatfsgen.py`, read back by `fatfsparse.py` |
