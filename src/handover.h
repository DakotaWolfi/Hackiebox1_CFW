/*
 * Handover record of the SD bootloader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    HANDOVER_ACTIVE = 0,
    HANDOVER_TRIAL = 1,
    HANDOVER_HELPER = 2,
} handover_kind_t;

typedef struct
{
    bool valid;   /* started by the SD bootloader */
    uint8_t slot; /* slot number 0..8 */
    uint8_t kind; /* handover_kind_t */
    uint32_t bootloader_word;
    bool magic; /* the magic was there, valid or not (start banner) */
} handover_t;

/* Copies the record out of SRAM and clears its magic. Runs before bss is
 * zeroed, so the result is kept in a section that is not cleared. */
void handover_capture(void);

const handover_t *handover_get(void);

#endif
