/*
 * Host test of esp32/main/assets.c.
 *
 *   test gate IMAGE WANT         the wear levelling check of a partition
 *                                image: WANT 1 accepted, 0 refused
 *   test files FAT DIR NEW       FatFs drive 1 on the plain FAT image FAT
 *                                (changed in place), made from DIR; NEW a
 *                                DER file for an upload
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "flashdisk.h"
#include "assets_host.h"

#define SECTOR 4096u

static FILE *img;
static long img_size;
static int failures;

static void check(bool ok, const char *what)
{
    printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

/* --- the partition (gate) and drive 1 (files) on the image file --- */

bool part_read(uint32_t off, void *p, uint32_t n)
{
    return fseek(img, (long)off, SEEK_SET) == 0 && fread(p, 1, n, img) == n;
}

/* The ROM's crc32_le: reflected CRC-32, the start value and the result
 * inverted. */
uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
        {
            crc = crc & 1 ? crc >> 1 ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

DSTATUS flashdisk_status(void)
{
    return img ? 0 : STA_NOINIT;
}

DRESULT flashdisk_read(BYTE *buf, LBA_t sector, UINT count)
{
    return part_read(sector * SECTOR, buf, count * SECTOR) ? RES_OK : RES_ERROR;
}

DRESULT flashdisk_write(const BYTE *buf, LBA_t sector, UINT count)
{
    bool ok = fseek(img, (long)(sector * SECTOR), SEEK_SET) == 0 && fwrite(buf, 1, count * SECTOR, img) == count * SECTOR;
    return ok ? RES_OK : RES_ERROR;
}

DRESULT flashdisk_ioctl(BYTE cmd, void *buf)
{
    switch (cmd)
    {
    case CTRL_SYNC:
    case CTRL_TRIM:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = (LBA_t)(img_size / SECTOR);
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

/* --- what diskio.c and ff.c need besides: no card here --- */

bool sd_ready(void)
{
    return false;
}
bool sd_identify(void)
{
    return false;
}
bool sd_read(uint32_t s, uint8_t *b, uint32_t c)
{
    return false;
}
bool sd_write(uint32_t s, const uint8_t *b, uint32_t c)
{
    return false;
}
void sd_erase(uint32_t f, uint32_t l)
{
}
uint32_t sd_sectors(void)
{
    return 0;
}
void sd_set_system_end(uint32_t s)
{
    printf("FAIL: card hook called for drive 1\n");
    exit(1);
}
uint32_t journal_sector(void)
{
    printf("FAIL: journal asked for drive 1\n");
    exit(1);
}
DWORD get_fattime(void)
{
    return (DWORD)(2026 - 1980) << 25 | 10u << 21 | 14u << 16;
}
int ff_mutex_create(int vol)
{
    return 1;
}
void ff_mutex_delete(int vol)
{
}
int ff_mutex_take(int vol)
{
    return 1;
}
void ff_mutex_give(int vol)
{
}

/* --- helpers --- */

static long load(const char *path, unsigned char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return -1;
    }
    long r = (long)fread(buf, 1, n, f);
    fclose(f);
    return r;
}

/* The copy name reads back as want (n bytes). */
static bool copy_is(const char *name, const unsigned char *want, long n)
{
    static unsigned char got[8192];
    return assets_size(name) == n && assets_read(name, 0, got, sizeof got) == n && !memcmp(got, want, (size_t)n);
}

/* A file on drive 1 as the firmware would leave it, n bytes of data. */
static void put(const char *path, const unsigned char *data, unsigned n)
{
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK || f_write(&f, data, n, &w) != FR_OK || f_close(&f))
    {
        printf("FAIL: setting up %s\n", path);
        exit(1);
    }
}

static bool absent(const char *path)
{
    FILINFO fi;
    return f_stat(path, &fi) == FR_NO_FILE;
}

static int files(const char *dir, const char *new_der)
{
    static unsigned char ca[8192], client[8192], key[8192], up[8192];
    char p[512];
    snprintf(p, sizeof p, "%s/CERT/CA.DER", dir);
    long nca = load(p, ca, sizeof ca);
    snprintf(p, sizeof p, "%s/CERT/CLIENT.DER", dir);
    long ncl = load(p, client, sizeof client);
    snprintf(p, sizeof p, "%s/CERT/PRIVATE.DER", dir);
    long nkey = load(p, key, sizeof key);
    long nup = load(new_der, up, sizeof up);

    printf("first start\n");
    check(assets_mount_host(), "mounted");
    check(copy_is("CFW/ca.der", ca, nca) && copy_is("CFW/client.der", client, ncl) && copy_is("CFW/PRIVATE.DER", key, nkey),
          "three copies equal to /CERT/");

    printf("upload of a client certificate\n");
    check(assets_replace("CFW/client.der", up, (unsigned)nup) == nup, "replaced");
    check(copy_is("CFW/client.der", up, nup), "the copy holds the upload");
    check(absent("1:/CFW/CLIENT.NEW"), "no new file left");

    printf("next start: an existing copy stays\n");
    check(assets_mount_host() && copy_is("CFW/client.der", up, nup), "the upload, not /CERT/");

    printf("power loss after the old copy went, before the rename\n");
    put("1:/CFW/PRIVATE.NEW", up, (unsigned)nup);
    f_unlink("1:/CFW/PRIVATE.DER");
    check(assets_mount_host() && copy_is("CFW/private.der", up, nup), "the new content");
    check(absent("1:/CFW/PRIVATE.NEW"), "new file renamed");

    printf("power loss while the new file was written\n");
    put("1:/CFW/CA.NEW", up, (unsigned)nup / 2);
    check(assets_mount_host() && copy_is("CFW/ca.der", ca, nca), "the old content");
    check(absent("1:/CFW/CA.NEW"), "the partial file deleted");

    printf("power loss in the first copy\n");
    f_unlink("1:/CFW/CA.DER");
    put("1:/CFW/CA.NEW", ca, (unsigned)nca - 10);
    check(assets_mount_host() && copy_is("CFW/ca.der", ca, nca), "copied again from /CERT/");

    printf("a CA under another name (key ca)\n");
    check(assets_size("CFW/myca.der") == -14, "missing at first");
    put("1:/CFW/MYCA.NEW", ca, (unsigned)nca);
    check(copy_is("CFW/myca.der", ca, nca), "completed when read");
    check(assets_delete("CFW/myca.der") == 0 && assets_size("CFW/myca.der") == -14, "deleted");
    check(assets_size("CFW/../CERT/CA.DER") == -14 && assets_replace("x/y.der", up, 10) == -1 &&
              assets_size("CERT/CA.DER") == -14 && assets_size("WWW/INDEX.HTM") == -14,
          "names outside /CFW/ refused");

    printf("the sound-prompt archive\n");
    static unsigned char sfx[100000], got[100000];
    snprintf(p, sizeof p, "%s/FW/SFX.BIN", dir);
    long nsfx = load(p, sfx, sizeof sfx);
    check(assets_size("fw/sfx.bin") == nsfx && assets_read("FW/SFX.BIN", 0, got, sizeof got) == nsfx &&
              !memcmp(got, sfx, (size_t)nsfx) && assets_read("FW/SFX.BIN", 4090, got, 100) == 100 &&
              !memcmp(got, sfx + 4090, 100),
          "read, also across a sector");
    check(assets_replace("FW/SFX.BIN", up, 10) == -1 && assets_delete("FW/SFX.BIN") == -1, "never written");

    printf("unmounted: the image is checked by fatfsparse.py\n");
    f_mount(NULL, "1:", 0);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        return 2;
    }
    img = fopen(argv[2], !strcmp(argv[1], "files") ? "r+b" : "rb");
    if (!img)
    {
        return 2;
    }
    fseek(img, 0, SEEK_END);
    img_size = ftell(img);
    if (!strcmp(argv[1], "gate"))
    {
        bool want = atoi(argv[3]) != 0, got = assets_wl_acceptable((uint32_t)img_size);
        printf("  %s: %s %s\n", got == want ? "ok" : "FAIL", argv[2], got ? "accepted" : "refused");
        return got != want;
    }
    if (!strcmp(argv[1], "files") && argc == 5)
    {
        int r = files(argv[3], argv[4]);
        fclose(img);
        return r;
    }
    return 2;
}
