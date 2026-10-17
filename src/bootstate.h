/*
 * Boot state file of the SD bootloader. Only for a firmware started by the SD
 * bootloader; all functions need the mounted card.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BOOTSTATE_H
#define BOOTSTATE_H

#include <stdbool.h>
#include <stdint.h>

/* The 32 bytes of a valid state file; false when the file
 * is missing or invalid or the firmware was not started by the SD
 * bootloader. Offsets: 4 active, 5 next, 6 kind, 7 tries, 8 task, 9 last
 * result, 10 helper slot, 12 rolled-back build (little-endian). */
bool bootstate_read(uint8_t s[32]);
/* next slot = slot, next kind 1, tries 0, last result 0; the
 * caller then restarts. */
bool bootstate_trial(uint8_t slot);
/* active slot = own slot, next slot 255, kind 0, tries 0,
 * last result 1. */
bool bootstate_confirm(void);
/* True while this start is an unconfirmed trial start (handover kind 1). */
bool bootstate_trial_pending(void);
/* Results of the confirmation checks of a trial start, reported where they
 * become known: the card mount, the codec configuration, the tag reader
 * test, the clock set from the first time request of this start. */
typedef enum
{
    BOOT_CHECK_CARD,
    BOOT_CHECK_CODEC,
    BOOT_CHECK_READER,
    BOOT_CHECK_TIME,
} boot_check_t;
void bootstate_report(boot_check_t c, bool ok);
/* Once per second from activity 1 while a trial is pending;
 * joined: the box is joined to a Wi-Fi network (connection state 3 or
 * more). Confirms when card, codec and reader are fine and the time was
 * applied or 60 s passed without a join; restarts without confirming (MCU
 * reset) when a check failed or after 300 s joined without confirmation. */
void bootstate_trial_poll(bool joined);

#endif
