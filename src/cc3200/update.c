/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "bootstate.h"
#include "debug.h"
#include "handover.h"
#include "net.h"
#include "sha256.h"
#include "lowram.h"
#include "sound.h"
#include "update.h"
#include "version.h"

#define TRAILER 176u /* build block + digest text */
#define DIGEST 64u

/* The install in progress; only valid while u_open (no initial value:
 * below the image, lowram.h). */
static struct
{
    unsigned slot; /* 1..3 */
    uint32_t length, pos;
    sha256_t sha;
    uint8_t tail[TRAILER]; /* the last 176 bytes */
    FIL f;
} u LOWRAM;
static bool u_open;

static volatile bool restart_pending;

static void slot_name(char *out, unsigned n, const char *ext)
{
    snprintf(out, 32, "/revvox/boot/ng-cfw%u.%s", n, ext);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* lowest cfw slot that is neither the running one nor the
 * helper slot and whose file is missing or carries the family marker CFW1
 * at offset 93 of its build block. Slot numbers of the state file and the
 * handover record: cfw1..3 = 3..5. */
unsigned update_free_slot(void)
{
    static FIL f;
    const handover_t *h = handover_get();
    uint8_t s[32];
    unsigned helper = bootstate_read(s) ? s[10] : 255;
    for (unsigned n = 1; n <= 3; n++)
    {
        if ((h->valid && h->slot == 2 + n) || helper == 2 + n)
        {
            continue;
        }
        char name[32];
        slot_name(name, n, "bin");
        if (f_open(&f, name, FA_READ) != FR_OK)
        {
            return n;
        }
        UINT got = 0;
        uint8_t marker[4] = {0};
        FSIZE_t z = f_size(&f);
        bool ok = z >= TRAILER && f_lseek(&f, z - TRAILER + 93) == FR_OK && f_read(&f, marker, 4, &got) == FR_OK &&
                  got == 4 && !memcmp(marker, "CFW1", 4);
        f_close(&f);
        if (ok)
        {
            return n;
        }
    }
    return 0;
}

void update_abort(void)
{
    if (!u_open)
    {
        return;
    }
    char name[32];
    f_close(&u.f);
    slot_name(name, u.slot, "tmp");
    f_unlink(name);
    u_open = false;
    debug_printf("update: abort, %s deleted\n", name);
}

bool update_begin(uint32_t length)
{
    update_abort();
    /* before anything is written. */
    if (length < TRAILER || length >= 0x80000000u)
    {
        debug_printf("update: %lu B < %u, abort\n", (unsigned long)length, TRAILER);
        return false;
    }
    if (length > UPDATE_SLOT_MAX)
    {
        debug_printf("update: %lu B > %u, abort\n", (unsigned long)length,
                     UPDATE_SLOT_MAX);
        return false;
    }
    if (!handover_get()->valid)
    {
        debug_printf("update: no SD bootloader, abort\n");
        return false;
    }
    unsigned n = update_free_slot();
    if (!n)
    {
        debug_printf("update: no free slot, abort\n");
        return false;
    }
    char name[32];
    slot_name(name, n, "tmp"); /* never directly into the slot file */
    if (f_open(&u.f, name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        debug_printf("update: %s not created, abort\n", name);
        return false;
    }
    u_open = true;
    u.slot = n;
    u.length = length;
    u.pos = 0;
    sha256_init(&u.sha);
    debug_printf("update: item 3, %lu B into %s\n", (unsigned long)length, name);
    return true;
}

bool update_write(const uint8_t *p, size_t n)
{
    if (!u_open)
    {
        return false;
    }
    if (n > u.length - u.pos)
    {
        n = u.length - u.pos; /* surplus bytes are not processed */
    }
    if (!n)
    {
        return true;
    }
    UINT w = 0;
    if (f_write(&u.f, p, (UINT)n, &w) != FR_OK || w != n)
    {
        debug_printf("update: write FAILED at %lu\n", (unsigned long)u.pos);
        update_abort();
        return false;
    }
    /* The digest covers everything before the last 64 bytes. */
    uint32_t end = u.length - DIGEST;
    if (u.pos < end)
    {
        sha256_update(&u.sha, p, u.pos + n <= end ? n : end - u.pos);
    }
    uint32_t tail_start = u.length - TRAILER;
    for (size_t i = 0; i < n; i++)
    {
        if (u.pos + i >= tail_start)
        {
            u.tail[u.pos + i - tail_start] = p[i];
        }
    }
    u.pos += (uint32_t)n;
    return true;
}

static const char *check_build_block(const uint8_t *bb)
{
    if (le32(bb) != 0xBEAC0005u || le32(bb + 108) != 0xBEAC0005u || (bb[92] & 7) > 3)
    {
        return "build block invalid";
    }
    if (le32(bb + 12))
    {
        return "debug build";
    }
    uint32_t kind = le32(bb + 100);
    if (kind == 4)
    {
        debug_printf("update: kind 4 ok\n");
    }
    else if (kind != 2)
    {
        return "image kind not 2";
    }
    if (memcmp(bb + 93, "CFW1", 4))
    {
        return "no CFW1 marker";
    }
    if (le32(bb + 8) == CFW_BUILD_TIME)
    {
        return "running build";
    }
    return NULL;
}

update_result_t update_finish(void)
{
    if (!u_open)
    {
        return UPDATE_ABANDONED;
    }
    char tmp[32], slot[32];
    slot_name(tmp, u.slot, "tmp");
    slot_name(slot, u.slot, "bin");
    if (u.pos != u.length)
    {
        debug_printf("update: %lu of %lu B\n", (unsigned long)u.pos, (unsigned long)u.length);
        update_abort();
        return UPDATE_ABANDONED;
    }
    u_open = false;
    bool closed = f_close(&u.f) == FR_OK;

    /* lower-case hexadecimal text of the digest. */
    static const char hex[] = "0123456789abcdef";
    uint8_t d[32];
    char text[DIGEST];
    sha256_final(&u.sha, d);
    for (int i = 0; i < 32; i++)
    {
        text[2 * i] = hex[d[i] >> 4];
        text[2 * i + 1] = hex[d[i] & 15];
    }
    const char *why = !closed                                           ? "file not closed"
                      : memcmp(text, u.tail + TRAILER - DIGEST, DIGEST) ? "digest mismatch"
                                                                        : check_build_block(u.tail);
    if (why)
    {
        debug_printf("update: %s, abort\n", why);
        f_unlink(tmp);
        return UPDATE_ABANDONED;
    }
    /* only now the slot file is replaced. */
    FRESULT r = f_unlink(slot);
    if ((r != FR_OK && r != FR_NO_FILE) || f_rename(tmp, slot) != FR_OK)
    {
        debug_printf("update: %s not replaced, abort\n", slot);
        f_unlink(tmp);
        return UPDATE_ABANDONED;
    }
    debug_printf("update: build %lu in %s\n", (unsigned long)le32(u.tail + 8), slot);
    if (!bootstate_trial((uint8_t)(2 + u.slot)))
    {
        debug_printf("update: no state file, no trial\n");
        return UPDATE_ABANDONED;
    }
    restart_pending = true;
    return UPDATE_INSTALLED;
}

bool update_restart_pending(void)
{
    return restart_pending;
}

void update_restart(void)
{
    sound_park(500);
    net_shutdown();
    /* The retained state of the power handling is not stored yet. */
    debug_printf("update: restart\n");
    vTaskDelay(pdMS_TO_TICKS(100) + 1);
    MAP_PRCMMCUReset(true);
    for (;;)
        ;
}
