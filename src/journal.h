/*
 * Card journal /journal.bin on the serial flash.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#define JOURNAL_NONE 0xFFFFFFFFu

/* Right before the panics of a system area failure (version 1) and of a
 * second failure (version 2); replaces an earlier journal. */
void journal_write_content(const uint8_t uid[8]);
void journal_write_sector(uint32_t sector);
/* At start-up, once the network processor runs and before the card is
 * mounted: reads, deletes and keeps the journal. */
void journal_load(void);
/* The sector of a version 2 journal for the mount, else
 * JOURNAL_NONE. */
uint32_t journal_sector(void);
/* The UID of a version 1 journal, to be moved to BADBLOCK after the mount;
 * false if none. */
bool journal_content(uint8_t uid[8]);
/* moves that content file to BADBLOCK;
 * once, after a successful mount. */
void journal_apply_content(void);

#endif
