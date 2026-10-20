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
update and the server configuration (see "What the custom firmware does
beyond the original" below). The factory
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

## What the custom firmware does beyond the original

This section is for users. The custom firmware plays Tonies exactly like the
original. On top of that it has the functions below, and it solves some
things differently. The settings for them are made in teddyCloud with CFW
support (`teddycloud_cfw`): per Tonie in the Tonie's edit dialog, per box
under Tonieboxes, and as event tags. The box gets them with its usual check
with the server and keeps them on the SD card, so they also work offline.
A box without any of these settings behaves like the original firmware,
except for the three functions marked **always on**. The ESP32 box runs
the same firmware and has the same functions.

### Slap the box upside down: start over (always on)

> **Turn the box upside down (ears facing down, as for the original's
> restart with both ears) and slap it on either side. Playback jumps back to the very beginning of the Tonie, with the
> usual "chapter back" sound. With shuffle on, a new random order starts.**

The original firmware has no such gesture. There, a slap on the
upside-down box only moves one chapter, as on the upright box. Neither the
original documentation nor teddyCloud mention the gesture, so new users
will not find it on their own. It is handy for audio books and for Tonies
with many chapters, where slapping back chapter by chapter takes long. The
slap lock (below) stops this gesture too.

### Switch off with both ears (always on)

Press both ears together and let go within 2 seconds: the LED blinks lilac
once, playback stops (the position is kept as on removing the Tonie), and
the box switches off at once, even with a Tonie on it. The original firmware
has no off switch; it only switches itself off after its idle time. Holding
both ears longer still does what it does in the original (setup mode after
5 seconds, restart upside down after 10 seconds). The
function can be switched off per box.

### Any NFC tag as a Tonie (always on)

Besides real Tonies, the box accepts any ISO 15693 tag (for example NFC
stickers or cards of the same type). It reads only the tag's ID and never
writes to it. In teddyCloud you give the tag a content like any Tonie. It
can also be a normal Tonie, a Tonie with its own settings, or an event tag.

### Settings per Tonie

| Setting | Effect |
| --- | --- |
| Resume | The Tonie continues where it stopped, even when other Tonies played in between or the box was off. The original remembers only the Tonie played last. When the content has played to its end, the next start is from the beginning. |
| Shuffle | Contents with 2 to 99 chapters play every chapter once in a random order. Slapping moves through this order. Without resume, the order is kept as long as no other Tonie plays in between; with resume, it is always kept. A finished order, or the upside-down slap, starts a new one. |
| Sleep timer | After N minutes the box stops. During the first third the volume stays as it is, then it fades linearly to silence. Taking the Tonie off pauses the timer, putting it back continues it. Changing the volume with the ears during the fade fades from the new volume. |
| Volume limit | Its own limit for speaker and headphones, in all 16 volume steps (0 to 15), and it may be higher than the box's. The original only knows 4 levels per box. |
| LED | On, dimmed or off while this Tonie is on the box. |
| Locks | Slap lock, tilt lock and volume lock (below), only for this Tonie. A Tonie's setting wins over the box's in both directions. |

### Settings per box

Speaker and headphone limit (0 to 15), LED level, the three locks and the
switch-off with both ears. In the original, the server can only set the
limit in 4 levels and switch slapping and tilting on or off together.

The locks:

- **Slap lock:** slapping no longer changes the chapter (also not the
  upside-down slap).
- **Tilt lock:** tilting no longer winds forward or back.
- **Volume lock:** a short ear press no longer changes the volume. Holding
  an ear and the gestures with both ears still work.

A locked gesture does nothing at all: no sound, no blink, no chapter change.

### Event tags

Any tag (a spare Tonie, a Creative Tonie or an NFC sticker) can be made an
event tag in teddyCloud. Putting it on the box plays nothing; the LED
blinks turquoise once. Then:

- **For the next Tonie:** the actions are kept for 60 seconds. The next
  Tonie placed within that time takes them over and keeps them: switch
  resume or shuffle on, off or over, set a sleep timer (permanent, or only
  for this time), reset all its settings, switch its locks. Parents can thus
  change a Tonie's settings without a phone or computer.
- **Set with the ears:** while the event tag is on the box, the ears set the
  volume limit (both outputs are opened up to the maximum) or the LED level
  (each press: on, dimmed, off). The value counts when the tag is taken off,
  for the next Tonie or for the whole box. If a tag sets both, holding one
  ear for 3 seconds switches between volume and LED.
- **For the whole box:** switch the box's locks and the switch-off with both
  ears at once.

The box confirms with five blinks and a short tone pair per action:
turquoise and a rising tone for "on", lilac and a falling tone for "off",
alternating when the tag did both. The tones can be switched off per tag.

### Remote control

From teddyCloud, and through MQTT from Home Assistant: pause, continue,
stop, next and previous chapter, set the volume, play a content without a
Tonie on the box (by its ID), show the state, and look for a firmware
update now. While paused, the box counts as idle and switches off after its
idle time; with resume, it continues at the paused position later. The
state (playing, Tonie, chapter, volume) is updated live.

### Battery

The box reports the battery voltage, whether it is on the charging station
and whether the battery is low. teddyCloud shows it on the box's card.

### Server, setup and updates

- **Server address on the SD card.** The box reads its server (host, port,
  CA certificate, time server) from `/cfw/config.txt` or the setup page.
  It never contacts the manufacturer's servers on its own; nothing in the
  flash has to be patched to point it to teddyCloud.
- **Own setup page** (in several languages): Wi-Fi with a list of the
  networks around, server, certificate upload, and delete functions for
  the stored Wi-Fi networks, for all local contents and settings, and for
  the server settings. They replace the factory reset of the original.
- **Original firmware stays.** On the CC3200 box the custom firmware starts
  from the SD card; the original stays in the flash and can be started
  again at any time.
- **Updates through teddyCloud, safely.** A new version goes into a free
  slot and starts once as a trial. If it does not come up properly, the box
  goes back to the previous version on its own. teddyCloud offers release,
  beta and alpha channels and the download from GitHub.
- **Works with an unmodified teddyCloud.** Towards teddyCloud and the
  Boxine cloud the box presents itself as the original firmware 3.4.0, so
  passing requests on to the cloud keeps working. Only with CFW support
  switched on in teddyCloud are the functions above set from there.

### More robust

Many cases in which the original firmware crashes or restarts are handled
calmly: malformed answers from the server, broken or odd files on the SD
card, very long Wi-Fi names, unexpected inputs on the setup page. The box
ignores the input or discards the file and carries on.

### Not (yet) there

The factory reset through the ears does not return to a factory image;
use the delete functions of the setup page. Of the original's log records
the box sends only those teddyCloud evaluates.

## Legal

Toniebox, Tonie and tonies are trademarks of tonies GmbH (formerly Boxine
GmbH). This project is not affiliated with, endorsed or supported by tonies
GmbH; the names only say which devices the software works with.

Changing the firmware of a box, opening it or connecting to its debug port
is at your own risk: a box can become unusable, and it can affect claims
against the manufacturer. The software comes without any warranty (see the
licence). Using the manufacturer's cloud is subject to its terms of use.

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
