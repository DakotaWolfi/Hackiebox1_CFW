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
#include "crc32.h"
#include "debug.h"
#include "ext.h"
#include "handover.h"
#include "tick.h"

#define CONFIG_DIR "/revvox/boot"
#define KEY "\"stateFile\""

static bool confirmed;

/* the configuration's long name ngCfg.json has
 * no 8.3 form, so the card holds it under a generated short name such as
 * NGCFG~1.JSO; the box, which knows short names only, looks for that. */
static bool config_open(FIL *f)
{
    static DIR d;
    static FILINFO fi;
    char name[32];
    bool found = false;
    if (f_opendir(&d, CONFIG_DIR) != FR_OK)
    {
        return false;
    }
    while (!found && f_readdir(&d, &fi) == FR_OK && fi.fname[0])
    {
        size_t n = strlen(fi.fname);
        found = !(fi.fattrib & AM_DIR) && !strncmp(fi.fname, "NGCFG~", 6) && n > 4 &&
                !strcmp(fi.fname + n - 4, ".JSO");
    }
    f_closedir(&d);
    snprintf(name, sizeof name, CONFIG_DIR "/%s", fi.fname);
    return found && f_open(f, name, FA_READ) == FR_OK;
}

/* The value of the key stateFile in the bootloader configuration. A small
 * scanner instead of a JSON parser: the key, a colon, a quoted string. The
 * configuration is never written. */
static bool state_path(char *path, unsigned size)
{
    static FIL f;
    char buf[64];
    UINT n;
    unsigned matched = 0, len = 0;
    enum
    {
        KEYWORD,
        COLON,
        OPEN,
        VALUE
    } st = KEYWORD;
    bool ok = false;

    if (!config_open(&f))
    {
        return false;
    }
    while (!ok && f_read(&f, buf, sizeof buf, &n) == FR_OK && n)
    {
        for (UINT i = 0; i < n && !ok; i++)
        {
            char c = buf[i];
            switch (st)
            {
            case KEYWORD:
                matched = c == KEY[matched] ? matched + 1 : (c == KEY[0] ? 1 : 0);
                if (matched == sizeof KEY - 1)
                {
                    st = COLON;
                }
                break;
            case COLON:
            case OPEN:
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    break;
                }
                if (st == COLON && c == ':')
                {
                    st = OPEN;
                }
                else if (st == OPEN && c == '"')
                {
                    st = VALUE;
                }
                else
                {
                    st = KEYWORD, matched = 0;
                }
                break;
            case VALUE:
                if (c == '"')
                {
                    path[len] = 0;
                    ok = len > 0;
                    st = KEYWORD; /* empty value: look on */
                }
                else if (len + 1 < size)
                {
                    path[len++] = c;
                }
                else
                {
                    st = KEYWORD, matched = 0, len = 0;
                }
                break;
            }
        }
    }
    f_close(&f);
    return ok;
}

static bool valid(const uint8_t s[32])
{
    uint32_t crc;
    memcpy(&crc, s + 28, 4);
    return !memcmp(s, "NGS1", 4) && crc == crc32_ogg(0, s, 28);
}

bool bootstate_read(uint8_t s[32])
{
    static FIL f;
    char path[64];
    UINT n = 0;
    if (!handover_get()->valid || !state_path(path, sizeof path) || f_open(&f, path, FA_READ) != FR_OK)
    {
        return false;
    }
    bool ok = f_size(&f) == 32 && f_read(&f, s, 32, &n) == FR_OK && n == 32 && valid(s);
    f_close(&f);
    return ok;
}

/* Reads the state file, lets change() modify it and writes it whole, in
 * place when it exists with 32 bytes, else newly from the values in
 * effect. */
static bool update(void (*change)(uint8_t s[32], uint8_t arg), uint8_t arg)
{
    static FIL f;
    char path[64];
    uint8_t s[32];
    UINT n = 0;

    if (!handover_get()->valid || !state_path(path, sizeof path))
    {
        debug_printf("bootstate: no state file configured\n");
        return false;
    }
    bool in_place = f_open(&f, path, FA_READ | FA_WRITE) == FR_OK;
    if (in_place && (f_size(&f) != 32 || f_read(&f, s, 32, &n) != FR_OK || n != 32 || !valid(s)))
    {
        f_close(&f);
        in_place = false;
    }
    if (!in_place)
    {
        memset(s, 0, sizeof s);
        memcpy(s, "NGS1", 4);
        s[4] = 255; /* activeImg */
        s[5] = 255;
        s[10] = 255;
        if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        {
            debug_printf("bootstate: %s cannot be created\n", path);
            return false;
        }
    }
    change(s, arg);
    uint32_t crc = crc32_ogg(0, s, 28);
    memcpy(s + 28, &crc, 4);
    bool ok = f_lseek(&f, 0) == FR_OK && f_write(&f, s, 32, &n) == FR_OK && n == 32;
    ok = f_close(&f) == FR_OK && ok;
    debug_printf("bootstate: %s %s (active %u next %u kind %u result %u)\n", path, ok ? "written" : "NOT written",
                 s[4], s[5], s[6], s[9]);
    return ok;
}

static void set_trial(uint8_t s[32], uint8_t slot)
{
    s[5] = slot;
    s[6] = 1;
    s[7] = 0;
    s[9] = 0;
}

static void set_confirmed(uint8_t s[32], uint8_t slot)
{
    s[4] = slot;
    s[5] = 255;
    s[6] = 0;
    s[7] = 0;
    s[8] = 0;
    s[9] = 1;
}

bool bootstate_trial(uint8_t slot)
{
    return update(set_trial, slot);
}

bool bootstate_confirm(void)
{
    if (!bootstate_trial_pending())
    {
        return false;
    }
    confirmed = update(set_confirmed, handover_get()->slot);
    if (confirmed)
    {
        ext_trial_confirmed(); /* the hello answer reports it */
    }
    return confirmed;
}

bool bootstate_trial_pending(void)
{
    const handover_t *h = handover_get();
    return h->valid && h->kind == HANDOVER_TRIAL && !confirmed;
}

static uint8_t checks_ok, checks_failed; /* bit per boot_check_t */

static void __attribute__((noreturn)) restart_unconfirmed(const char *why)
{
    debug_printf("bootstate: trial not confirmed (%s, ok %x failed %x), restart\n", why, checks_ok, checks_failed);
    vTaskDelay(100);
    MAP_PRCMMCUReset(true);
    for (;;)
        ;
}

void bootstate_report(boot_check_t c, bool ok)
{
    if (ok)
    {
        checks_ok |= 1u << c;
    }
    else
    {
        checks_failed |= 1u << c;
    }
}

void bootstate_trial_poll(bool joined)
{
    static bool ever_joined;
    const uint8_t hw = 1u << BOOT_CHECK_CARD | 1u << BOOT_CHECK_CODEC | 1u << BOOT_CHECK_READER;
    if (!bootstate_trial_pending())
    {
        return;
    }
    uint32_t now = tick_ms();
    ever_joined |= joined;
    if (checks_failed & hw)
    {
        restart_unconfirmed("hardware check failed");
    }
    if ((checks_ok & hw) == hw && ((checks_ok & 1u << BOOT_CHECK_TIME) || (now >= 60000u && !ever_joined)))
    {
        if (!bootstate_confirm())
        {
            restart_unconfirmed("state file not written");
        }
        return;
    }
    if (joined && now >= 300000u)
    {
        restart_unconfirmed("no time after 300 s");
    }
}
