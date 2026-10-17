/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "sdfiles.h"
#include "debug.h"
#include "flashfs.h"
#include "journal.h"

#define NAME "/journal.bin"

static uint8_t rec[12];
static bool have_content;
static uint32_t sector = JOURNAL_NONE;

/* maximum size the record length, no access flags. */
static void put(const uint8_t *data, unsigned len)
{
    long r = flashfs_replace(NAME, data, len, false);
    debug_printf("journal: %u bytes %s\n", len, r == (long)len ? "written" : "NOT written");
}

void journal_write_content(const uint8_t uid[8])
{
    uint8_t b[12] = {0, 12, 0, 1};
    memcpy(b + 4, uid, 8);
    put(b, 12);
}

void journal_write_sector(uint32_t s)
{
    uint8_t b[8] = {0, 8, 0, 2, (uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16), (uint8_t)(s >> 24)};
    put(b, 8);
}

void journal_load(void)
{
    long n = flashfs_read(NAME, 0, rec, sizeof rec);
    if (n < 0)
    {
        return; /* the normal case, not reported */
    }
    flashfs_delete(NAME);
    if (n < 4)
    {
        debug_printf("journal: too short (%ld bytes), ignored\n", n);
        return;
    }
    unsigned len = (unsigned)rec[0] << 8 | rec[1], ver = (unsigned)rec[2] << 8 | rec[3];
    if (!((ver == 1 && len == 12) || (ver == 2 && len == 8)) || n < (long)len)
    {
        debug_printf("journal: invalid, length %u version %u, %ld B\n", len, ver, n);
        return;
    }
    if (ver == 1)
    {
        have_content = true;
    }
    else
    {
        sector = (uint32_t)rec[4] | (uint32_t)rec[5] << 8 | (uint32_t)rec[6] << 16 | (uint32_t)rec[7] << 24;
    }
    debug_printf("journal: version %u found\n", ver);
}

uint32_t journal_sector(void)
{
    return sector;
}

bool journal_content(uint8_t uid[8])
{
    if (have_content)
    {
        memcpy(uid, rec + 4, 8);
    }
    return have_content;
}

/* the content file of a version 1
 * journal goes to BADBLOCK, named as in the trash; deleted when that
 * fails. After a successful mount. */
void journal_apply_content(void)
{
    uint8_t uid[8];
    if (!journal_content(uid))
    {
        return;
    }
    have_content = false;
    sd_move_aside(uid, "BADBLOCK");
}
