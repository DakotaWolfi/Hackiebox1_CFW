/*
 * The assets partition of the ESP32 box: a FAT12 file system
 * behind ESP-IDF's wear levelling, version 2, sector and page size 4096
 * (ESP-IDF documentation, "Wear Levelling API"), here FatFs drive "1:".
 *
 * The original firmware's files stay as they are: the firmware writes only
 * in /CFW/, where it keeps its copies of the TLS files of /CERT/ and uses
 * nothing else for its connections. Of the other files it reads only the
 * sound-prompt archive /FW/SFX.BIN. A missing copy is made from /CERT/ at
 * every start; an upload on the setup page replaces a copy. A copy is
 * replaced as NAME.NEW, written completely, then renamed over NAME.DER; a
 * NAME.NEW found next to a missing NAME.DER is the new content when it is
 * one complete DER element, and is renamed then.
 *
 * ESP-IDF's wear levelling driver formats the partition anew when it finds
 * a state it does not accept (another version, both copies broken), which
 * would destroy the box's certificates. So the state is checked here first
 * and the partition is used only when the driver will take it as it is.
 *
 * The file part runs on the host as well (esp32/tests/assets) with a plain
 * FAT image as drive 1, the state check with images of wl_fatfsgen.py.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ff.h"
#include "der.h"
#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "wear_levelling.h"
#include "debug.h"
#include "esp32.h"
#include "flashdisk.h"
#else
#include "assets_host.h"
#endif

#define SECTOR 4096u
#define RECORD 16u
#define STATE_LEN 64u
#define STATE_CRC_LEN 60u /* up to the CRC (version 2) */
#define WL_VERSION 2u

typedef struct
{
    uint32_t dummy_pos, max_pos, move_count, erase_cycles, max_erase_cycles, block_size, version, device_id;
    uint32_t reserved[7];
    uint32_t crc;
} wl_state_t;

#ifdef ESP_PLATFORM
#if CONFIG_WL_SECTOR_SIZE != 4096
#error "the assets partition has 4096-byte sectors"
#endif

static const esp_partition_t *part;
static wl_handle_t wl = WL_INVALID_HANDLE;

static bool part_read(uint32_t off, void *p, uint32_t n)
{
    return part && esp_partition_read(part, off, p, n) == ESP_OK;
}

static uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t n)
{
    return esp_rom_crc32_le(crc, p, n);
}
#endif

/* --- the wear levelling state --- */

/* True when ESP-IDF's driver will mount the partition without formatting
 * it: at least one copy of the state is valid, and every valid copy has
 * version 2 and the layout this partition size gives with 4096-byte
 * sectors. The driver repairs one broken copy from the other. */
bool assets_wl_acceptable(uint32_t part_size)
{
    uint32_t sectors = part_size / SECTOR;
    uint32_t state_size = SECTOR;
    if (state_size < STATE_LEN + sectors * RECORD)
    {
        state_size = (STATE_LEN + sectors * RECORD + SECTOR - 1) / SECTOR * SECTOR;
    }
    if (part_size < 2 * state_size + 3 * SECTOR)
    {
        return false;
    }
    uint32_t data = part_size - 2 * state_size - SECTOR; /* before the states and the configuration */
    uint32_t max_pos = data / SECTOR;                    /* 1 + the sectors without the dummy */
    unsigned valid = 0;
    for (unsigned i = 0; i < 2; i++)
    {
        wl_state_t s;
        if (!part_read(data + i * state_size, &s, sizeof s) ||
            crc32_le(0xFFFFFFFFu, (const uint8_t *)&s, STATE_CRC_LEN) != s.crc)
        {
            continue;
        }
        if (s.version != WL_VERSION || s.max_pos != max_pos || s.block_size != SECTOR || s.dummy_pos >= max_pos)
        {
            return false;
        }
        valid++;
    }
    return valid > 0;
}

#ifdef ESP_PLATFORM
/* --- FatFs drive 1 (flashdisk.h) --- */

DSTATUS flashdisk_status(void)
{
    return wl != WL_INVALID_HANDLE ? 0 : STA_NOINIT;
}

DRESULT flashdisk_read(BYTE *buf, LBA_t sector, UINT count)
{
    return wl_read(wl, sector * SECTOR, buf, count * SECTOR) == ESP_OK ? RES_OK : RES_ERROR;
}

DRESULT flashdisk_write(const BYTE *buf, LBA_t sector, UINT count)
{
    if (wl_erase_range(wl, sector * SECTOR, count * SECTOR) != ESP_OK)
    {
        return RES_ERROR;
    }
    return wl_write(wl, sector * SECTOR, buf, count * SECTOR) == ESP_OK ? RES_OK : RES_ERROR;
}

DRESULT flashdisk_ioctl(BYTE cmd, void *buf)
{
    switch (cmd)
    {
    case CTRL_SYNC:
    case CTRL_TRIM: /* nothing to release in the wear levelling */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = wl_size(wl) / SECTOR;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = SECTOR;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
#endif

/* --- the files in /CFW/ --- */

static FATFS fs;
static bool mounted;

/* "1:/DIR/NAME.EXT" for "DIR/NAME.EXT"; ext "NEW" gives the new file of a
 * replacement. Only /CFW/ and, for reading, /FW/SFX.BIN. */
static bool path_of(const char *rel, const char *ext, bool write, char *p, size_t n)
{
    const char *name = strchr(rel, '/');
    bool cfw = name && name - rel == 3 && !strncasecmp(rel, "CFW", 3);
    if (!cfw && (write || ext || strcasecmp(rel, "FW/SFX.BIN")))
    {
        return false;
    }
    name++;
    size_t len = strlen(name);
    if (!len || len > 12 || strchr(name, '/') || strchr(name, ':') || name[0] == '.')
    {
        return false;
    }
    int w = ext ? snprintf(p, n, "1:/CFW/%.*s.%s", (int)strcspn(name, "."), name, ext) : snprintf(p, n, "1:/%s", rel);
    if (w < 0 || (size_t)w >= n)
    {
        return false;
    }
    for (char *c = p; *c; c++)
    {
        *c = (char)toupper((unsigned char)*c);
    }
    return true;
}

static bool exists(const char *path)
{
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

/* One complete DER element, its size the file's (a new file's content is
 * complete only then). */
static bool der_complete(const char *path)
{
    FIL f;
    uint8_t head[4];
    UINT n = 0;
    if (f_open(&f, path, FA_READ) != FR_OK)
    {
        return false;
    }
    bool ok = f_read(&f, head, sizeof head, &n) == FR_OK && f_size(&f) && der_element_size(head, n) == f_size(&f);
    f_close(&f);
    return ok;
}

/* The new file renamed over the old one; the old one goes first, since a
 * FAT rename does not replace. */
static bool commit(const char *path, const char *path_new)
{
    FRESULT r = f_unlink(path);
    if (r != FR_OK && r != FR_NO_FILE)
    {
        return false;
    }
    return f_rename(path_new, path) == FR_OK;
}

/* A replacement cut short by a power loss: with the copy missing, a
 * complete new file is its content; any other new file goes. */
static void settle(const char *path, const char *path_new)
{
    if (!exists(path_new))
    {
        return;
    }
    if (!exists(path) && der_complete(path_new) && commit(path, path_new))
    {
        debug_printf("assets: %s completed\n", path);
        return;
    }
    f_unlink(path_new);
}

static bool write_new(const char *path_new, const void *buf, unsigned n)
{
    FIL f;
    UINT w = 0;
    if (f_open(&f, path_new, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    bool ok = f_write(&f, buf, n, &w) == FR_OK && w == n;
    return f_close(&f) == FR_OK && ok;
}

long assets_replace(const char *name, const void *buf, unsigned n)
{
    char path[24], path_new[24];
    if (!mounted || !path_of(name, NULL, true, path, sizeof path) || !path_of(name, "NEW", true, path_new, sizeof path_new))
    {
        return -1;
    }
    settle(path, path_new);
    bool ok = write_new(path_new, buf, n) && commit(path, path_new);
    debug_printf("assets: %s replaced: %s\n", path, ok ? "ok" : "FAILED");
    return ok ? (long)n : -1;
}

long assets_size(const char *name)
{
    char path[24], path_new[24];
    FILINFO fi;
    if (!mounted || !path_of(name, NULL, false, path, sizeof path))
    {
        return -14;
    }
    if (path_of(name, "NEW", true, path_new, sizeof path_new))
    {
        settle(path, path_new);
    }
    return f_stat(path, &fi) == FR_OK ? (long)fi.fsize : -14;
}

long assets_read(const char *name, uint32_t offset, void *buf, unsigned n)
{
    char path[24];
    FIL f;
    UINT got = 0;
    if (assets_size(name) < 0 || !path_of(name, NULL, false, path, sizeof path) || f_open(&f, path, FA_READ) != FR_OK)
    {
        return -14;
    }
    bool ok = f_lseek(&f, offset) == FR_OK && f_read(&f, buf, n, &got) == FR_OK;
    f_close(&f);
    return ok ? (long)got : -1;
}

long assets_delete(const char *name)
{
    char path[24], path_new[24];
    if (!mounted || !path_of(name, NULL, true, path, sizeof path) || !path_of(name, "NEW", true, path_new, sizeof path_new))
    {
        return -1;
    }
    f_unlink(path_new);
    FRESULT r = f_unlink(path);
    return r == FR_OK || r == FR_NO_FILE ? 0 : -1;
}

/* /CERT/name copied to /CFW/name through the new file. */
static bool copy_from_cert(const char *name)
{
    static uint8_t chunk[1024];
    char from[24], rel[16], path[24], path_new[24];
    FIL in, out;
    snprintf(from, sizeof from, "1:/CERT/%s", name);
    snprintf(rel, sizeof rel, "CFW/%s", name);
    if (!path_of(rel, NULL, true, path, sizeof path) || !path_of(rel, "NEW", true, path_new, sizeof path_new) ||
        f_open(&in, from, FA_READ) != FR_OK)
    {
        return false;
    }
    bool ok = f_open(&out, path_new, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if (ok)
    {
        UINT r = 0, w = 0;
        while (ok && f_read(&in, chunk, sizeof chunk, &r) == FR_OK && r)
        {
            ok = f_write(&out, chunk, r, &w) == FR_OK && w == r;
        }
        ok = f_close(&out) == FR_OK && ok && f_size(&in) == f_size(&out);
    }
    f_close(&in);
    return ok && commit(path, path_new);
}

/* before the first TLS connection, the missing copies. */
static void copies_make(void)
{
    static const char *const names[3] = {"CA.DER", "CLIENT.DER", "PRIVATE.DER"};
    FRESULT r = f_mkdir("1:/CFW");
    if (r != FR_OK && r != FR_EXIST)
    {
        debug_printf("assets: /CFW not made (%d)\n", r);
        return;
    }
    for (unsigned i = 0; i < 3; i++)
    {
        char rel[16], path[24], path_new[24];
        snprintf(rel, sizeof rel, "CFW/%s", names[i]);
        path_of(rel, NULL, true, path, sizeof path);
        path_of(rel, "NEW", true, path_new, sizeof path_new);
        settle(path, path_new);
        if (exists(path))
        {
            continue;
        }
        bool ok = copy_from_cert(names[i]);
        debug_printf("assets: %s copied from /CERT: %s\n", path, ok ? "ok" : "FAILED");
    }
}

static bool files_start(void)
{
    FRESULT r = f_mount(&fs, "1:", 1);
    mounted = r == FR_OK;
    debug_printf("assets: mount %d, FAT type %d, sector %u\n", r, fs.fs_type, (unsigned)fs.ssize);
    if (mounted)
    {
        copies_make();
    }
    return mounted;
}

#ifdef ESP_PLATFORM
bool assets_mount(void)
{
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "assets");
    if (!part || part->erase_size != SECTOR || !assets_wl_acceptable(part->size))
    {
        /* Never handed to the driver: it might format the partition. */
        debug_printf("assets: wear levelling state NOT accepted, partition unused\n");
        return false;
    }
    esp_err_t e = wl_mount(part, &wl);
    if (e != ESP_OK)
    {
        debug_printf("assets: wear levelling mount FAILED (%d)\n", e);
        wl = WL_INVALID_HANDLE;
        return false;
    }
    return files_start();
}
#else
bool assets_mount_host(void)
{
    return files_start();
}
#endif
