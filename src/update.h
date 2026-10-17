/*
 * Installing item 3, the own firmware image, into an SD slot under the SD
 * bootloader.
 *
 * The cloud activity streams the body of an answer 200 into it:
 *   update_begin(length) -> update_write(...)* -> update_finish()
 * or update_abort() when the connection fails. Every false or
 * UPDATE_ABANDONED means: the item is abandoned, nothing further is asked
 * for in this start. UPDATE_INSTALLED means: no further item,
 * the online round counts as successful, and activity 1
 * restarts the box with update_restart() once update_restart_pending().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_SLOT_MAX 212992u /* trailer included */

typedef enum
{
    UPDATE_ABANDONED,
    UPDATE_INSTALLED,
} update_result_t;

/* the cfw slot 1..3 that receives a new image, 0 when none is
 * free (an image offered as item 3 is then abandoned). Needs
 * the card. */
unsigned update_free_slot(void);

#ifdef CFW_ESP32
/* the User-Agent of the original ESP firmware of the box. */
const char *update_original_agent(void);
#endif

/* Announced content length of the answer 200;
 * false: abandoned before anything was written. */
bool update_begin(uint32_t length);
/* Body bytes in order; bytes beyond the announced length are ignored.
 * False: abandoned (the partial file is gone). */
bool update_write(const uint8_t *p, size_t n);
/* After the last byte: digest, build block, family marker and build number
 * checks, then the slot file and the trial start in the boot state file. */
update_result_t update_finish(void);
/* The connection failed or the download was given up: the partial file is
 * deleted. Harmless when nothing is open. */
void update_abort(void);

/* A new image is in its slot and marked for its trial start. */
bool update_restart_pending(void);
/* park the audio output, network shutdown, record,
 * 100 ms, reset of the application processor with its peripherals. Only
 * from activity 1: the network shutdown waits for the cloud activity to
 * end its work. */
void update_restart(void) __attribute__((noreturn));

#endif
