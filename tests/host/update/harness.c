/* Host harness for update.c, sha256.c and bootstate.c: FatFs on a card
 * directory, images made by tools/mkimage.py (run.sh).
 * usage: update_test CARD IMAGES-DIR
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DIR DIR_HOST /* the host type; DIR below is the FatFs one */
#include <dirent.h>
#undef DIR
#include <sys/stat.h>
#include <unistd.h>
#include "update.c"
#include "sha256.c"
#include "bootstate.c"
#include "crc32.c"

static const char *root, *images;
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

/* --- stubs --- */
static uint32_t sim_ms;
void vTaskDelay(TickType_t t) { sim_ms += t; }
uint32_t tick_ms(void) { return sim_ms; }
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}
static handover_t ho = {true, 3, HANDOVER_ACTIVE, 0};
const handover_t *handover_get(void) { return &ho; }
void MAP_PRCMMCUReset(bool s)
{
    (void)s;
    printf("    | MCU reset\n");
    exit(failures ? 1 : 0);
}
bool sound_park(uint32_t ms)
{
    (void)ms;
    return true;
}
void net_shutdown(void) {}
static int trial_confirmations;
void ext_trial_confirmed(void) { trial_confirmations++; }

static void full(const char *p, char *q) { snprintf(q, 600, "%s%s", root, p); }
/* FF_USE_LFN 0: a name without an 8.3 form cannot be opened */
static bool short_name(const char *p)
{
    const char *n = strrchr(p, '/') ? strrchr(p, '/') + 1 : p, *dot = strchr(n, '.');
    size_t base = dot ? (size_t)(dot - n) : strlen(n);
    return base >= 1 && base <= 8 && (!dot || (strlen(dot + 1) <= 3 && !strchr(dot + 1, '.')));
}
FRESULT f_open(FIL *f, const char *p, BYTE m)
{
    char q[600];
    if (!short_name(p))
    {
        return FR_INVALID_NAME;
    }
    full(p, q);
    f->fp = fopen(q, (m & FA_CREATE_ALWAYS) ? "w+b" : (m & FA_WRITE) ? "r+b"
                                                                     : "rb");
    if (!f->fp)
    {
        return FR_NO_FILE;
    }
    fseek(f->fp, 0, SEEK_END);
    f->size = ftell(f->fp);
    rewind(f->fp);
    return FR_OK;
}
FRESULT f_read(FIL *f, void *b, UINT n, UINT *g)
{
    *g = fread(b, 1, n, f->fp);
    return FR_OK;
}
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w)
{
    *w = fwrite(b, 1, n, f->fp);
    long p = ftell(f->fp);
    if ((FSIZE_t)p > f->size)
    {
        f->size = p;
    }
    return FR_OK;
}
FRESULT f_lseek(FIL *f, FSIZE_t o) { return fseek(f->fp, o, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
FRESULT f_close(FIL *f)
{
    if (f->fp)
    {
        fclose(f->fp);
    }
    f->fp = NULL;
    return FR_OK;
}
FRESULT f_unlink(const char *p)
{
    char q[600];
    full(p, q);
    return unlink(q) ? FR_NO_FILE : FR_OK;
}
FRESULT f_opendir(DIR *d, const char *p)
{
    char q[600];
    full(p, q);
    d->d = opendir(q);
    return d->d ? FR_OK : FR_NO_PATH;
}
/* lists only the names that have an 8.3 form, as FatFs without LFN shows
 * them: the card directory holds the short names */
FRESULT f_readdir(DIR *d, FILINFO *fi)
{
    struct dirent *e;
    while ((e = readdir((DIR_HOST *)d->d)) && (e->d_name[0] == '.' || !short_name(e->d_name)))
        ;
    fi->fname[0] = 0;
    if (e)
    {
        snprintf(fi->fname, sizeof fi->fname, "%s", e->d_name);
        fi->fattrib = e->d_type == DT_DIR ? AM_DIR : 0;
    }
    return FR_OK;
}
FRESULT f_closedir(DIR *d)
{
    if (d->d)
    {
        closedir((DIR_HOST *)d->d);
    }
    return FR_OK;
}
FRESULT f_rename(const char *a, const char *b)
{
    char q[600], r[600];
    full(a, q);
    full(b, r);
    struct stat st;
    if (!stat(r, &st))
    {
        return FR_EXIST;
    }
    return rename(q, r) ? FR_DENIED : FR_OK;
}

/* --- helpers --- */
static uint8_t *load(const char *dir, const char *name, size_t *len)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f)
    {
        *len = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len ? *len : 1);
    if (fread(b, 1, *len, f) != *len)
    {
        *len = 0;
    }
    fclose(f);
    return b;
}

static bool exists(const char *card_path)
{
    char q[600];
    struct stat st;
    full(card_path, q);
    return !stat(q, &st);
}

/* Streams an image file with an announced length, in chunks, plus surplus. */
static update_result_t stream(const char *name, int64_t announce, size_t chunk, size_t stop_after, size_t surplus)
{
    size_t len;
    uint8_t *b = load(images, name, &len);
    uint32_t a = announce < 0 ? (uint32_t)len : (uint32_t)announce;
    if (!update_begin(a))
    {
        free(b);
        return 99;
    }
    size_t total = len + surplus, sent = 0;
    uint8_t *x = calloc(total, 1);
    memcpy(x, b, len);
    while (sent < total && sent < stop_after)
    {
        size_t k = total - sent < chunk ? total - sent : chunk;
        if (!update_write(x + sent, k))
        {
            free(b);
            free(x);
            return UPDATE_ABANDONED;
        }
        sent += k;
    }
    free(b);
    free(x);
    if (sent < total && stop_after != (size_t)-1 && stop_after < len)
    {
        update_abort();
        return 98;
    }
    return update_finish();
}

static void hexd(const uint8_t *d, char *out)
{
    for (int i = 0; i < 32; i++)
    {
        sprintf(out + 2 * i, "%02x", d[i]);
    }
}

static void test_sha(void)
{
    printf("T sha256 vectors\n");
    sha256_t s;
    uint8_t d[32];
    char h[65];
    sha256_init(&s);
    sha256_final(&s, d);
    hexd(d, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty");
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, d);
    hexd(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&s);
    sha256_update(&s, m, strlen(m));
    sha256_final(&s, d);
    hexd(d, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "448 bits");
    static uint8_t a[1000];
    memset(a, 'a', sizeof a);
    sha256_init(&s);
    for (int i = 0, k = 1; i < 1000000; i += k, k = k % 997 + 1)
    {
        sha256_update(&s, a, (size_t)(i + k <= 1000000 ? k : 1000000 - i));
    }
    sha256_final(&s, d);
    hexd(d, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "million a, odd chunks");
}

static void state(uint8_t s[32])
{
    memset(s, 0, 32);
    CHECK(bootstate_read(s), "state file valid");
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        return 2;
    }
    root = argv[1];
    images = argv[2];
    test_sha();

    printf("T free slot\n");
    CHECK(update_free_slot() == 2, "running cfw1, cfw2 missing: slot 2");
    uint8_t s[32];
    state(s);
    s[10] = 4;
    uint32_t crc = crc32_ogg(0, s, 28);
    memcpy(s + 28, &crc, 4);
    {
        FIL f;
        UINT w;
        f_open(&f, "/revvox/boot/ngstate.bin", FA_WRITE);
        f_write(&f, s, 32, &w);
        f_close(&f);
    }
    CHECK(update_free_slot() == 3, "helper slot cfw2 (4) excluded: slot 3");
    s[10] = 255;
    crc = crc32_ogg(0, s, 28);
    memcpy(s + 28, &crc, 4);
    {
        FIL f;
        UINT w;
        f_open(&f, "/revvox/boot/ngstate.bin", FA_WRITE);
        f_write(&f, s, 32, &w);
        f_close(&f);
    }

    printf("T lengths refused before writing\n");
    CHECK(!update_begin(175), "175 bytes");
    CHECK(!update_begin(212993), "212993 bytes");
    CHECK(!update_begin(0x80000000u), "2^31 bytes");
    CHECK(!exists("/revvox/boot/ng-cfw2.tmp"), "no temporary file");

    printf("T refused images\n");
    CHECK(stream("bad_digest.bin", -1, 97, (size_t)-1, 0) == UPDATE_ABANDONED, "wrong digest abandoned");
    CHECK(stream("same_build.bin", -1, 512, (size_t)-1, 0) == UPDATE_ABANDONED, "running build number abandoned");
    CHECK(stream("no_marker.bin", -1, 512, (size_t)-1, 0) == UPDATE_ABANDONED, "no family marker abandoned");
    CHECK(stream("kind3.bin", -1, 512, (size_t)-1, 0) == UPDATE_ABANDONED, "image kind 3 abandoned");
    CHECK(stream("debug.bin", -1, 512, (size_t)-1, 0) == UPDATE_ABANDONED, "debug flag abandoned");
    CHECK(stream("good.bin", -1, 1400, 3000, 0) == 98, "connection lost: aborted");
    {
        size_t len;
        free(load(images, "good.bin", &len));
        CHECK(stream("good.bin", (int64_t)len + 10, 1400, (size_t)-1, 0) == UPDATE_ABANDONED, "short body abandoned");
    }
    CHECK(!exists("/revvox/boot/ng-cfw2.tmp") && !exists("/revvox/boot/ng-cfw2.bin"), "no temporary or slot file left");
    CHECK(!update_restart_pending(), "no restart pending");
    state(s);
    CHECK(s[5] == 255 && s[6] == 0, "state file unchanged");

    printf("T older image installed (with 100 surplus bytes)\n");
    CHECK(stream("good.bin", -1, 97, (size_t)-1, 100) == UPDATE_INSTALLED, "installed");
    size_t a, b;
    uint8_t *x = load(images, "good.bin", &a), *y = load(root, "revvox/boot/ng-cfw2.bin", &b);
    CHECK(x && y && a == b && !memcmp(x, y, a), "slot file equals the download, surplus not written");
    free(x);
    free(y);
    CHECK(!exists("/revvox/boot/ng-cfw2.tmp"), "temporary file renamed");
    state(s);
    CHECK(s[4] == 255 && s[5] == 4 && s[6] == 1 && s[7] == 0 && s[9] == 0 && s[10] == 255, "trial start: next cfw2, kind 1, tries 0, other fields kept");
    CHECK(update_restart_pending(), "restart pending");

    printf("T confirmation as cfw2 after the trial start\n");
    ho.slot = 4;
    ho.kind = HANDOVER_TRIAL;
    restart_pending = false;
    CHECK(bootstate_trial_pending(), "trial pending");
    bootstate_report(BOOT_CHECK_CARD, true);
    bootstate_report(BOOT_CHECK_CODEC, true);
    sim_ms = 61000;
    bootstate_trial_poll(false);
    state(s);
    CHECK(s[5] == 4, "reader not reported yet: not confirmed");
    bootstate_report(BOOT_CHECK_READER, true);
    bootstate_trial_poll(false);
    state(s);
    CHECK(s[4] == 4 && s[5] == 255 && s[6] == 0 && s[7] == 0 && s[9] == 1, "confirmed after 60 s without Wi-Fi: active cfw2, result 1");
    CHECK(trial_confirmations == 1, "the hello answer learns of the confirmation");
    CHECK(!bootstate_trial_pending(), "no trial pending");
    CHECK(update_free_slot() == 1, "running cfw2: slot 1 (cfw1 has the marker)");

    printf("T no free slot\n");
    ho.slot = 3;
    ho.kind = HANDOVER_ACTIVE;
    {
        FIL f;
        UINT w;
        uint8_t z[300] = {0};
        f_open(&f, "/revvox/boot/ng-cfw3.bin", FA_WRITE | FA_CREATE_ALWAYS);
        f_write(&f, z, 300, &w);
        f_close(&f);
    }
    {
        FIL f;
        UINT w;
        uint8_t z[300] = {0};
        f_open(&f, "/revvox/boot/ng-cfw2.bin", FA_WRITE | FA_CREATE_ALWAYS);
        f_write(&f, z, 300, &w);
        f_close(&f);
    }
    CHECK(update_free_slot() == 0, "cfw2 and cfw3 hold foreign images: none");
    CHECK(!update_begin(5000), "item 3 refused without a slot");

    printf("T restart\n");
    printf("%s\n", failures ? "update: FAILURES" : "update: all ok");
    update_restart();
}
