/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "handover.h"
#include "crc32.h"

#define HANDOVER_ADDR 0x20003FE0u
#define HANDOVER_MAGIC 0x3148474Eu /* "NGH1" little-endian */

/* Not in .bss: handover_capture() runs before bss is cleared. */
static handover_t record __attribute__((section(".data")));

void handover_capture(void)
{
    uint8_t raw[32];
    volatile uint8_t *src = (volatile uint8_t *)HANDOVER_ADDR;
    uint32_t magic, crc, word;

    for (int i = 0; i < 32; i++)
    {
        raw[i] = src[i];
    }

    memcpy(&magic, raw, 4);
    memcpy(&crc, raw + 28, 4);
    memcpy(&word, raw + 8, 4);

    record.magic = magic == HANDOVER_MAGIC;
    record.valid = magic == HANDOVER_MAGIC && crc32_ogg(0, raw, 28) == crc && raw[4] <= 8 && raw[5] <= 2;
    if (record.valid)
    {
        record.slot = raw[4];
        record.kind = raw[5];
        record.bootloader_word = word;
    }
    /* A later reset must not see the record again. */
    for (int i = 0; i < 4; i++)
    {
        src[i] = 0;
    }
}

const handover_t *handover_get(void)
{
    return &record;
}
