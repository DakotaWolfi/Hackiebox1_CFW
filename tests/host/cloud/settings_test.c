/* Host test for settings.c: /settings.cfg language write and country
 * code. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "simplelink.h"
#include "settings.c"

static int failures;
#define CHECK(c, ...)                       \
    do                                      \
    {                                       \
        if (!(c))                           \
        {                                   \
            failures++;                     \
            printf("  FAIL: " __VA_ARGS__); \
        }                                   \
        else                                \
            printf("  ok: " __VA_ARGS__);   \
        printf("\n");                       \
    } while (0)
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}

/* one flash file, /settings.cfg; /fw/sfx.bin missing */
static uint8_t cfg_file[64];
static long cfg_len = -1;
static int writes, open_mode = -1;
static bool is_cfg(const unsigned char *n) { return !strcmp((const char *)n, "/settings.cfg"); }
long sl_FsOpen(unsigned char *n, unsigned long m, unsigned long *t, long *fh)
{
    (void)t;
    if (!is_cfg(n))
    {
        return -11;
    }
    if (m == FS_MODE_OPEN_READ && cfg_len < 0)
    {
        return -11;
    }
    if (m == FS_MODE_OPEN_WRITE && cfg_len < 0)
    {
        return -11;
    }
    open_mode = (int)m;
    if (m != FS_MODE_OPEN_READ)
    {
        cfg_len = 0;
    }
    *fh = 1;
    return 0;
}
long sl_FsRead(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    (void)fh;
    if ((long)o >= cfg_len)
    {
        return -1;
    }
    if ((long)(o + l) > cfg_len)
    {
        l = (unsigned long)(cfg_len - (long)o);
    }
    memcpy(b, cfg_file + o, l);
    return (long)l;
}
long sl_FsWrite(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    (void)fh;
    writes++;
    memcpy(cfg_file + o, b, l);
    if ((long)(o + l) > cfg_len)
    {
        cfg_len = (long)(o + l);
    }
    return (long)l;
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
    return 0;
}
short sl_FsGetInfo(unsigned char *n, unsigned long t, SlFsFileInfo_t *i)
{
    (void)t;
    if (!is_cfg(n) || cfg_len < 0)
    {
        return -11;
    }
    memset(i, 0, sizeof *i);
    i->FileLen = (uint32_t)cfg_len;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char cc[2];
    printf("S1 file with language 1, country DE, factory 2, field 2\n");
    static const uint8_t f1[] = {0x08, 0x01, 0x10, 0x05, 0x2A, 0x02, 'D', 'E', 0x30, 0x02};
    memcpy(cfg_file, f1, sizeof f1);
    cfg_len = sizeof f1;
    settings_load();
    CHECK(settings_language() == 1 && settings_country(cc) && cc[0] == 'D' && cc[1] == 'E', "language 1, country DE");
    settings_set_language(3);
    static const uint8_t w1[] = {0x08, 0x03, 0x2A, 0x02, 'D', 'E', 0x30, 0x02};
    CHECK(writes == 1 && cfg_len == sizeof w1 && !memcmp(cfg_file, w1, sizeof w1) && settings_language() == 3, "rewritten: 1=3, 5=DE, 6=2, field 2 left out, one write");
    settings_set_language(4);
    CHECK(writes == 1, "index 4 not stored");
    printf("S2 no file\n");
    cfg_len = -1;
    writes = 0;
    settings_load();
    CHECK(!settings_country(cc), "no country code");
    settings_set_language(2);
    static const uint8_t w2[] = {0x08, 0x02, 0x2A, 0x02, 'E', 'U', 0x30, 0x02};
    CHECK(writes == 1 && open_mode == (int)FS_MODE_OPEN_CREATE(64, 1) && cfg_len == sizeof w2 && !memcmp(cfg_file, w2, sizeof w2), "created: 1=2, 5=EU, 6=2");
    CHECK(settings_country(cc) && cc[0] == 'E' && cc[1] == 'U', "country now EU");
    printf("S3 file breaking off inside field 5\n");
    static const uint8_t f3[] = {0x08, 0x01, 0x2A, 0x02, 'D'};
    memcpy(cfg_file, f3, sizeof f3);
    cfg_len = sizeof f3;
    settings_load();
    CHECK(!settings_country(cc), "decoding failed: no country code");
    printf("S4 empty country code\n");
    static const uint8_t f4[] = {0x08, 0x00, 0x2A, 0x00};
    memcpy(cfg_file, f4, sizeof f4);
    cfg_len = sizeof f4;
    settings_load();
    CHECK(settings_country(cc) && !cc[0] && !cc[1], "present but empty");
    printf("\n%d failure(s)\n", failures);
    return failures != 0;
}
