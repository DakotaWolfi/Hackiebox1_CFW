/* Host test: retained state encoding (pwrstate.c) and the card journal
 * with the BADBLOCK move (journal.c).
 * usage: power_test CARD
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pwrstate.c"
#include "simplelink.h"
#include "journal.c"
#include "clock.h"

static const char *root;
static int failures;
#define CHECK(c, ...)                       \
    do                                      \
    {                                       \
        if (!(c))                           \
        {                                   \
            failures++;                     \
            printf("  FAIL: " __VA_ARGS__); \
            printf("\n");                   \
        }                                   \
        else                                \
        {                                   \
            printf("  ok: " __VA_ARGS__);   \
            printf("\n");                   \
        }                                   \
    } while (0)

static uint32_t sim_ms = 5000;
uint32_t tick_ms(void) { return sim_ms; }
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}

/* serial flash: one file in memory */
static uint8_t flash[16];
static long flash_len = -1;
long sl_FsOpen(unsigned char *n, unsigned long m, unsigned long *t, long *fh)
{
    (void)n;
    (void)t;
    *fh = 1;
    if (m == FS_MODE_OPEN_READ && flash_len < 0)
    {
        return -11;
    }
    if (m != FS_MODE_OPEN_READ)
    {
        flash_len = 0;
    }
    return 0;
}
long sl_FsRead(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    (void)fh;
    long n = flash_len - (long)o;
    if (n > (long)l)
    {
        n = l;
    }
    if (n < 0)
    {
        n = 0;
    }
    memcpy(b, flash + o, n);
    return n;
}
long sl_FsWrite(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    (void)fh;
    memcpy(flash + o, b, l);
    flash_len = o + l;
    return l;
}
short sl_FsClose(long fh, unsigned char *a, unsigned char *b, unsigned long c)
{
    (void)fh;
    (void)a;
    (void)b;
    (void)c;
    return 0;
}
short sl_FsDel(unsigned char *n, unsigned long t)
{
    (void)n;
    (void)t;
    flash_len = -1;
    return 0;
}
short sl_FsGetInfo(unsigned char *n, unsigned long t, SlFsFileInfo_t *i)
{
    (void)n;
    (void)t;
    if (flash_len < 0)
    {
        return -11;
    }
    i->FileLen = (uint32_t)flash_len;
    return 0;
}

/* FatFs on host files */
static void full(const char *p, char *q) { snprintf(q, 600, "%s/%s", root, p); }
FRESULT f_stat(const char *p, FILINFO *fi)
{
    char q[600];
    struct stat st;
    full(p, q);
    if (stat(q, &st))
    {
        return FR_NO_FILE;
    }
    memset(fi, 0, sizeof *fi);
    fi->fsize = st.st_size;
    return FR_OK;
}
FRESULT f_unlink(const char *p)
{
    char q[600];
    full(p, q);
    return unlink(q) ? FR_NO_FILE : FR_OK;
}
FRESULT f_rename(const char *a, const char *b)
{
    char q[600], r[600];
    full(a, q);
    full(b, r);
    return rename(q, r) ? FR_DENIED : FR_OK;
}
FRESULT f_mkdir(const char *p)
{
    char q[600];
    full(p, q);
    return mkdir(q, 0755) ? FR_EXIST : FR_OK;
}
FRESULT f_open(FIL *f, const char *p, BYTE m)
{
    (void)f;
    (void)p;
    (void)m;
    return FR_NO_FILE;
}
FRESULT f_read(FIL *f, void *b, UINT n, UINT *g)
{
    (void)f;
    (void)b;
    (void)n;
    *g = 0;
    return FR_DISK_ERR;
}
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w)
{
    (void)f;
    (void)b;
    (void)n;
    *w = 0;
    return FR_DISK_ERR;
}
FRESULT f_lseek(FIL *f, FSIZE_t o)
{
    (void)f;
    (void)o;
    return FR_DISK_ERR;
}
FRESULT f_close(FIL *f)
{
    (void)f;
    return FR_OK;
}
FRESULT f_chmod(const char *p, BYTE a, BYTE m)
{
    (void)p;
    (void)a;
    (void)m;
    return FR_OK;
}
FRESULT f_opendir(DIR *d, const char *p)
{
    (void)d;
    (void)p;
    return FR_NO_PATH;
}
FRESULT f_readdir(DIR *d, FILINFO *fi)
{
    (void)d;
    (void)fi;
    return FR_DISK_ERR;
}
FRESULT f_closedir(DIR *d)
{
    (void)d;
    return FR_OK;
}
FRESULT f_getfree(const char *p, DWORD *n, FATFS **fs)
{
    (void)p;
    (void)n;
    (void)fs;
    return FR_DISK_ERR;
}

static bool exists(const char *p)
{
    char q[600];
    struct stat st;
    full(p, q);
    return !stat(q, &st);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        return 2;
    }
    root = argv[1];
    clock_set(0x12345678u);

    printf("T retained state encoding\n");
    pwrstate_t a = {2, 3, 11, 4, true, false, true, 2, true, true, 0}, b;
    uint8_t uid[8] = {0x47, 0x3F, 0x0D, 0x0D, 0x50, 0x03, 0x04, 0xE0};
    a.resume = pwrstate_resume(uid, 1000);
    CHECK(a.resume == (0xF47u | 500u << 12), "resume value: tag B0 + low 4 bits of B1, half block");
    CHECK(pwrstate_resume(uid, 1) == 0 && (pwrstate_resume(uid, 40000) >> 12) == 16383, "block 1 nothing, 40000 kept as 16383");
    uint32_t w[2];
    pwrstate_encode(&a, false, w);
    CHECK(!(w[1] & (1u << 30)) && !(w[1] & PWRSTATE_SOFT_OFF) && (w[0] || w[1]), "bit 30 clear, not all zero");
    CHECK(pwrstate_decode(w, &b) && !memcmp(&a, &b, sizeof a), "round trip");
    pwrstate_encode(&a, true, w);
    CHECK((w[1] & PWRSTATE_SOFT_OFF) && pwrstate_decode(w, &b) && b.step_speaker == 11, "soft-off marker");
    pwrstate_t d = {3, 3, 8, 8, true, true, true, 0, false, false, 0};
    pwrstate_encode(&d, false, w);
    CHECK(w[0] || w[1], "defaults are not all zero");
    uint32_t fault[2] = {15, 1u << 30}, zero[2] = {0, 0};
    CHECK(!pwrstate_decode(fault, &b) && !pwrstate_decode(zero, &b), "fault record and zero are no state");
    uint32_t other[2] = {0x12345678u, 0x8000ABCDu}, nobit29[2] = {0, 0x80000123u}, ours[2] = {0, 0xA0000123u};
    CHECK(!pwrstate_decode(other, &b) && !pwrstate_decode(nobit29, &b) && pwrstate_decode(ours, &b),
          "garbage or another firmware's record is no state (bits 13-28 zero, bit 29 set)");

    printf("T journal: version 1 moves the content to BADBLOCK\n");
    uint8_t u[8] = {0x47, 0x3F, 0x0D, 0x0D, 0x50, 0x03, 0x04, 0xE0};
    char path[32], q[600];
    sd_content_path(u, path);
    sd_make_dirs(path);
    full(path, q);
    FILE *f = fopen(q, "w");
    fputs("x", f);
    fclose(f);
    journal_write_content(u);
    CHECK(flash_len == 12 && flash[1] == 12 && flash[3] == 1, "journal written, 12 bytes, version 1");
    journal_load();
    CHECK(flash_len < 0, "journal deleted at load");
    journal_apply_content();
    CHECK(!exists(path), "content file gone");
    CHECK(exists("BADBLOCK/780/123456E0.043"), "BADBLOCK/780/123456E0.043 (bad-block file name)");

    printf("T journal: version 2 sector, short record ignored\n");
    journal_write_sector(0x01020304);
    journal_load();
    CHECK(journal_sector() == 0x01020304, "sector journal");
    flash_len = 8;
    flash[0] = 0;
    flash[1] = 12;
    flash[2] = 0;
    flash[3] = 1;
    have_content = false;
    journal_load();
    CHECK(!journal_content(u) && flash_len < 0, "8 bytes with length 12 ignored and deleted");

    printf("%s\n", failures ? "power: FAILURES" : "power: all ok");
    return failures != 0;
}
