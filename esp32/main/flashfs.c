/*
 * Flash files (flashfs.h) on the ESP32 box:
 *
 *   /cert/<name>   the firmware's copies in /CFW/<NAME> of the assets
 *                  partition; the files of the original
 *                  firmware in /CERT/ are never changed
 *   /fw/sfx.bin    /FW/SFX.BIN of the assets partition, read only: the
 *                  sound-prompt archive
 *   /fw/, /sys/, /www/   otherwise missing: the firmware brings its own
 *                  setup page and reads nothing else of the original
 *   other names    blobs in the NVS namespace "cfw", keyed by the name
 *                  without the leading slash (settings.cfg, journal.bin)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp32.h"
#include "flashfs.h"
#include "port.h"

typedef enum
{
    F_ASSET,
    F_NVS,
    F_NONE,
} kind_t;

static kind_t kind(const char *name, char *mapped, size_t n)
{
    if (!strncmp(name, "/cert/", 6))
    {
        if (strlen(name + 6) + 4 >= n)
        {
            return F_NONE;
        }
        strcpy(mapped, "CFW/");
        strcat(mapped, name + 6);
        return F_ASSET;
    }
    if (!strcmp(name, "/fw/sfx.bin"))
    {
        strcpy(mapped, "FW/SFX.BIN"); /* written never: assets.c refuses */
        return F_ASSET;
    }
    if (!strncmp(name, "/fw/", 4) || !strncmp(name, "/sys/", 5) || !strncmp(name, "/www/", 5))
    {
        return F_NONE;
    }
    const char *k = name[0] == '/' ? name + 1 : name;
    if (strlen(k) > 15 || strchr(k, '/'))
    {
        return F_NONE;
    }
    strcpy(mapped, k);
    return F_NVS;
}

long flashfs_size(const char *name)
{
    char m[48];
    switch (kind(name, m, sizeof m))
    {
    case F_ASSET:
        return assets_size(m);
    case F_NVS:
        return nvsfile_size(m);
    default:
        return FLASHFS_MISSING;
    }
}

long flashfs_read(const char *name, uint32_t offset, void *buf, unsigned n)
{
    char m[48];
    switch (kind(name, m, sizeof m))
    {
    case F_ASSET:
        return assets_read(m, offset, buf, n);
    case F_NVS:
        return nvsfile_read(m, offset, buf, n);
    default:
        return FLASHFS_MISSING;
    }
}

long flashfs_write(const char *name, const void *buf, unsigned n)
{
    char m[48];
    switch (kind(name, m, sizeof m))
    {
    case F_ASSET:
        return assets_replace(m, buf, n); /* always written in full first */
    case F_NVS:
        return nvsfile_write(m, buf, n);
    default:
        return -1;
    }
}

long flashfs_replace(const char *name, const void *buf, unsigned n, bool failsafe)
{
    (void)failsafe;
    return flashfs_write(name, buf, n);
}

long flashfs_delete(const char *name)
{
    char m[48];
    switch (kind(name, m, sizeof m))
    {
    case F_ASSET:
        return assets_delete(m);
    case F_NVS:
        return nvsfile_delete(m);
    default:
        return -1;
    }
}
