# Toniebox custom firmware

Two box generations, one code base: `src/` is shared, `src/cc3200/` is the
CC3200 platform (this README), `esp32/` the ESP32-S3 box as an ESP-IDF
project (`esp32/README.md`, GPL-3.0-or-later). The repository is
`Toniebox1_CFW` (until 2026-10-14 `CC3200_CustomFirmware`).

Firmware for the first-generation Toniebox (TI CC3200). It is based on
observing the box's behaviour, measurements on the hardware and on the
network, and trying things out. It does what the original firmware does
at the functional level, plus a teddyCloud extension (settings files,
event tags, remote commands) and the parts for the SD bootloader, the
update and the server configuration. The factory
reset is replaced by the delete functions of the setup page. Of the log records the firmware sends what teddyCloud
evaluates (events, tag, firmware, playback and volume records); the rest
is not planned. Much of the code has not run on a device yet.

Licence: GPL-3.0-or-later (`COPYING`), for both platforms (the ESP32 build
uses ESP-IDF, Apache-2.0). Third-party code under `third_party/` keeps its
own licence: libopus 1.1 (BSD-3-Clause), FatFs R0.15a (FatFs licence,
BSD-1-Clause style), FreeRTOS (MIT), TI SimpleLink host driver
(BSD-3-Clause). The TI CC3200 SDK is used from outside the repository. Files under `third_party/` are not edited: a patched
vendored file is a copy under `src/` (`src/fatfs/ff.c`, the patches marked
`FF_TB_PATCHES`), identical to the original except the patch.

## Building

Requirements: GNU Arm Embedded toolchain 10-2020-q4, TI CC3200 SDK 1.5.0,
Python 3, GNU make. The defaults point at the copies in the neighbouring
`hackiebox_cfw_ng_cfw` repository; override them in `config.local.mk`:

```make
ARM_GCC_DIR = /opt/gcc-arm-none-eabi-10-2020-q4-major/bin
CC3200_SDK  = /opt/ti/cc3200-sdk
```

`make` builds `build/cfw-slot.bin`: the image (loaded at 0x20004000) with
the build block and the SHA-256 digest text at its end,
usable as `/revvox/boot/ng-cfwN.bin` of the SD bootloader and
as an application slot of the original bootloader. The build number is the
commit time; the version is `CFW_<CFW_VERSION>`.

Build switches (`make VAR=value`; the Makefile does not track flag
changes, so `make clean` after changing one):

| Switch | Default | Meaning |
| --- | --- | --- |
| `CFW_DEBUG` | 1 | 2 adds the verbose bring-up diagnostics (`debug_verbose`) on UART0 |
| `CFW_VERBOSE` | (none) | modules with `debug_verbose` output at level 1, e.g. `CFW_VERBOSE="net cloud"` (all at once do not fit the SRAM) |
| `CFW_LOG_UART` | 0 | 1 copies the binary log frames to UART0 |
| `CFW_HIBERNATE` | 0 | 1 uses hibernation for the off state and the hibernate restart; 0 an MCU reset and a low-power wait (not yet tried on a device) |
| `CFW_OPUS_SMALL` | 1 | `cwrs.c` without its pulse-count tables, about 5 KB less SRAM |

`make stackcheck` prints the worst-case stack per activity from the
`.su` files and the call graph (needs a linked build).

## Setup page

The setup page (Wi-Fi with the scan list, server host and
port, certificate upload) is a file on the SD card: copy
`card/cfw/setup.htm` to `/cfw/setup.htm` on the card. In setup mode the
firmware then serves it itself on port 80 (`src/page.c`, which lists the
interface); without the file the box offers the vendor setup page of the
network processor (Wi-Fi only).

The page has its own version, apart from the firmware's:
`cfw-page-version` in its head, raised in the
same commit as any change to the page, and `cfw-page-requires`, the oldest
firmware it works with (raise it when the page starts to use something
only a newer firmware has). `tools/release.sh` ships the page as it is, so
a release without a change to the page does not make teddyCloud deliver
it again. The page shows its version and the firmware's (`GET
/version.txt`).

## Code style

The conventions of the Revvox projects (teddycloud):

- `make format` formats the own sources with the `.clang-format` in the
  root, `make format-check` only checks: Allman braces, 4 spaces, braces
  around every body. This is the style VS Code applies on save.
  `third_party/`, `src/fatfs/` and `src/ffconf.h` (FatFs template layout)
  are not formatted.
- Functions and variables in snake_case with the module as prefix, types
  as typedefs named `<name>_t`, macros and enum values in upper case.
- Vendored code is not edited in place (see Licence above).
- Commits follow Conventional Commits, `type(scope): summary`. Style and
  refactoring commits keep the behaviour.

## Tests

`sh tests/host/all.sh <content file with three chapters>` (for example
`tools/taftest` output) builds and runs every host test with the address
and undefined-behaviour sanitizers: player and EXT features, cloud
requests and settings files, log stream and log connection, setup-mode
rules, the update install, the retained state and the setup page server.
Nothing in them replaces a test on a device.

## Notes

`docs/` holds investigations that are not (yet) decisions: `docs/tls-sni.md`
(why the box sends no SNI and what sending one would entail).

## Memory budget

The image runs from SRAM: at most 212992 bytes of image, and
all code, data and stacks share the 240 KB from 0x20004000; the 16 KB
below it hold the no-init buffers (section `.lowram`) once the handover
record is read. Current build: about 160 KB code, 98 KB data
and stacks (Opus decoder state 17.9 KB, player stack 20 KB for the Opus
worst case of packet-loss concealment, log buffer 6.9 KB), about 1.6 KB
left above the 1.75 KB main stack. Every further feature has to pay for itself
in SRAM.
