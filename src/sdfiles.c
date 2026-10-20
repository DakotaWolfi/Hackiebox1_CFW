/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "clock.h"
#include "debug.h"
#include "sdfiles.h"
#include "taf.h"

/* FAT timestamps from the clock in UTC, 0 before 1980. */
DWORD get_fattime(void)
{
    uint32_t t = clock_now();
    if (t < 315532800u)
    {
        return 0;
    }
    civil_t c;
    clock_to_civil(t, &c);
    return (DWORD)(c.year - 1980) << 25 | (DWORD)c.month << 21 | (DWORD)c.day << 16 | (DWORD)c.hour << 11 |
           (DWORD)c.minute << 5 | (DWORD)(c.second / 2);
}

uint32_t sd_fat_time(uint16_t fdate, uint16_t ftime)
{
    civil_t c = {(uint32_t)(fdate >> 9) + 1980, (uint32_t)(fdate >> 5 & 15), (uint32_t)(fdate & 31),
                 (uint32_t)(ftime >> 11), (uint32_t)(ftime >> 5 & 63), (uint32_t)(ftime & 31) * 2};
    if (!fdate || c.month < 1 || c.month > 12 || c.day < 1 || c.hour > 23 || c.minute > 59 || c.second > 59)
    {
        return 0;
    }
    return clock_from_civil(&c);
}

void sd_content_path(const uint8_t uid[8], char *path)
{
    /* created and looked up as upper-case 8.3 names. */
    static const char hex[] = "0123456789ABCDEF";
    strcpy(path, "CONTENT/00000000/00000000");
    for (int i = 0; i < 8; i++)
    {
        char *d = path + 8 + 2 * i + (i >= 4 ? 1 : 0);
        d[0] = hex[uid[i] >> 4];
        d[1] = hex[uid[i] & 15];
    }
}

bool sd_make_dirs(const char *file_path)
{
    char p[64];
    snprintf(p, sizeof p, "%s", file_path);
    for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/'))
    {
        *s = 0;
        FRESULT r = f_mkdir(p);
        *s = '/';
        if (r != FR_OK && r != FR_EXIST)
        {
            return false;
        }
    }
    return true;
}

/* to TRASHCAN or BADBLOCK (dir). */
void sd_move_aside(const uint8_t uid[8], const char *dir)
{
    char path[32], name[48];
    FILINFO fi;
    sd_content_path(uid, path);
    FRESULT r = f_stat(path, &fi);
    if (r == FR_NO_FILE || r == FR_NO_PATH)
    {
        return;
    }
    if (r != FR_OK)
    {
        f_unlink(path);
        return;
    }
    uint32_t t = clock_now();
    for (unsigned i = 0; i < 16; i++)
    {
        snprintf(name, sizeof name, "%s/%02lX%X/%06lX%02X.%02X%X", dir, (unsigned long)(t % 256), i,
                 (unsigned long)(t / 256) & 0xFFFFFF, uid[7], uid[6], uid[5] % 16);
        r = f_stat(name, &fi);
        if (r == FR_OK)
        {
            continue;
        }
        if ((r == FR_NO_FILE || r == FR_NO_PATH) && sd_make_dirs(name) && f_rename(path, name) == FR_OK)
        {
            debug_printf("sd: %s moved to %s\n", path, name);
            return;
        }
        break;
    }
    debug_printf("sd: %s deleted, not moved to %s\n", path, dir);
    f_unlink(path);
}

/* down to two levels below the start, paths up to 37
 * characters. */
static void delete_tree(char *path, unsigned depth)
{
    if (depth <= 2)
    {
        DIR d;
        FILINFO fi;
        if (f_opendir(&d, path) == FR_OK)
        {
            size_t n = strlen(path);
            while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
            {
                if (n + 1 + strlen(fi.fname) > 37)
                {
                    continue;
                }
                path[n] = '/';
                strcpy(path + n + 1, fi.fname);
                if (fi.fattrib & AM_DIR)
                {
                    delete_tree(path, depth + 1);
                }
                else
                {
                    f_unlink(path);
                }
                path[n] = 0;
            }
            f_closedir(&d);
        }
    }
    f_unlink(path);
}

/* Setup page: every content
 * directory except the sound prompts (CONTENT/0000000<s>), the
 * trash and the extension's files (tag and box settings, states). */
void sd_delete_content(void)
{
    static const char *const trees[2] = {"TRASHCAN", "CFW/EXT"};
    char path[40];
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, "CONTENT") == FR_OK)
    {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        {
            if (strncmp(fi.fname, "0000000", 7))
            {
                snprintf(path, sizeof path, "CONTENT/%s", fi.fname);
                delete_tree(path, 1);
            }
        }
        f_closedir(&d);
    }
    for (unsigned i = 0; i < 2; i++)
    {
        strcpy(path, trees[i]);
        delete_tree(path, 0);
    }
    debug_printf("sd: content, trash and extension files deleted\n");
}

static uint32_t free_clusters(uint32_t *total)
{
    FATFS *fs;
    DWORD n = 0;
    if (f_getfree("", &n, &fs) != FR_OK)
    {
        return 0;
    }
    *total = fs->n_fatent - 2;
    return n;
}

static bool hex8(const char *s)
{
    if (strlen(s) != 8)
    {
        return false;
    }
    for (int i = 0; i < 8; i++)
    {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') || (s[i] >= 'a' && s[i] <= 'f')))
        {
            return false;
        }
    }
    return true;
}

/* Cache candidate (simplified scan: the validation follows with the
 * cache report); returns false when there is no candidate. */
static uint8_t *scratch; /* 4096 bytes of the caller of sd_room */

static bool oldest(char *found)
{
    uint8_t *buf = scratch;
    DIR top, sub;
    FILINFO a, b;
    uint32_t best = 0xFFFFFFFFu;
    bool any = false;
    if (f_opendir(&top, "CONTENT") != FR_OK)
    {
        return false;
    }
    while (f_readdir(&top, &a) == FR_OK && a.fname[0])
    {
        if (!(a.fattrib & AM_DIR) || !hex8(a.fname) || !strncmp(a.fname, "0000000", 7))
        {
            continue; /* not a content directory, or a prompt directory */
        }
        char dir[24];
        snprintf(dir, sizeof dir, "CONTENT/%.8s", a.fname);
        if (f_opendir(&sub, dir) != FR_OK)
        {
            continue;
        }
        while (f_readdir(&sub, &b) == FR_OK && b.fname[0])
        {
            if ((b.fattrib & AM_DIR) || !hex8(b.fname))
            {
                continue;
            }
            uint32_t age = sd_fat_time(b.fdate, b.ftime);
            if (!age)
            {
                age = 0xFFFFFFFFu;
                char p[40];
                FIL f;
                UINT got;
                taf_header_t h;
                snprintf(p, sizeof p, "%s/%.8s", dir, b.fname);
                if (f_open(&f, p, FA_READ) == FR_OK)
                {
                    if (f_read(&f, buf, TAF_BLOCK, &got) == FR_OK && got == TAF_BLOCK &&
                        taf_header_decode(buf, &h))
                    {
                        age = h.audio_id;
                    }
                    f_close(&f);
                }
            }
            if (age < best)
            {
                best = age;
                snprintf(found, 40, "%s/%.8s", dir, b.fname);
                any = true;
            }
        }
        f_closedir(&sub);
    }
    f_closedir(&top);
    return any;
}

bool sd_room(uint32_t threshold, uint32_t target, volatile bool *stop, uint8_t *work)
{
    scratch = work;
    uint32_t total = 0, n = free_clusters(&total);
    if (target >= total)
    {
        return false;
    }
    if (n >= threshold)
    {
        return true;
    }
    char path[40] = "TRASHCAN";
    delete_tree(path, 0);
    debug_verbose("sd: trash deleted for room\n");
    while (free_clusters(&total) < target)
    {
        if (stop && *stop)
        {
            return false;
        }
        char victim[40];
        if (!oldest(victim))
        {
            return true;
        }
        f_unlink(victim);
        FILINFO fi;
        debug_printf("sd: oldest %s deleted for room\n", victim);
        if (f_stat(victim, &fi) == FR_OK)
        {
            return true;
        }
    }
    return true;
}

bool sd_room_for(uint32_t z, volatile bool *stop, uint8_t *work)
{
    FATFS *fs;
    DWORD n;
    if (f_getfree("", &n, &fs) != FR_OK)
    {
        return false;
    }
    uint32_t bytes = (uint32_t)fs->csize * 512u;
    uint32_t need = 2 * (z / bytes + 1);
    return sd_room(need, need, stop, work);
}

bool sd_outdated(const char *path)
{
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_HID);
}

void sd_set_outdated(const char *path, bool on)
{
    f_chmod(path, on ? AM_HID : 0, AM_HID);
}
