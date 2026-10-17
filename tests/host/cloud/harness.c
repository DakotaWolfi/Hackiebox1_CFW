/* Host harness for cloud.c: scripted server answers, captured requests. */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <utime.h>
#include "cloud.c"
#include "ext.c"
#include "crc32.c"
#include "sha256.c"
static int recv_calls;
static void (*recv_hook)(int);

static const char *root;
static uint32_t sim_ms;
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

/* --- RTOS --- */
typedef struct hq
{
    uint8_t buf[16][128];
    unsigned size, head, tail;
} hq_t;
static hq_t qs[2];
static int nq;
QueueHandle_t xQueueCreateStatic(UBaseType_t l, UBaseType_t s, uint8_t *b, StaticQueue_t *q)
{
    (void)l;
    (void)b;
    (void)q;
    qs[nq].size = s;
    return &qs[nq++];
}
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t t)
{
    (void)t;
    memcpy(q->buf[q->head++ % 16], it, q->size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *it, TickType_t t)
{
    (void)t;
    if (q->head == q->tail)
    {
        return pdFALSE;
    }
    memcpy(it, q->buf[q->tail++ % 16], q->size);
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->head - q->tail; }
void vTaskDelay(TickType_t t) { sim_ms += t; }
TaskHandle_t xTaskCreateStatic(void (*f)(void *), const char *n, uint32_t d, void *p, UBaseType_t pr, StackType_t *s, StaticTask_t *tc)
{
    (void)f;
    (void)n;
    (void)d;
    (void)p;
    (void)pr;
    (void)s;
    (void)tc;
    return NULL;
}
uint32_t tick_ms(void) { return sim_ms; }
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}

/* --- others --- */
static uint8_t state = 4;
static uint32_t session = 1;
static int last_state_req = -1, sync_res = -99;
uint8_t net_state(void) { return state; }
uint32_t net_session(void) { return session; }
uint32_t net_sp_version(void) { return 34471936; }
void net_cloud_state(uint32_t s, uint8_t to)
{
    (void)s;
    last_state_req = to;
    state = to;
}
static int first_result = -99;
void net_cloud_first_result(uint32_t s, int e)
{
    (void)s;
    first_result = e;
}
static handover_t ho = {true, 3, 0, 0};
const handover_t *handover_get(void) { return &ho; }
void ledui_request(led_request_t r) { (void)r; }
void log_round_done(uint32_t s) { (void)s; }
static unsigned free_slot = 2;
static int up_begun, up_aborted, up_finished, up_fail_at = -1, boot_time_reports;
static uint32_t up_len, up_got, up_sum;
static update_result_t up_result = UPDATE_INSTALLED;
unsigned update_free_slot(void) { return free_slot; }
bool update_begin(uint32_t l)
{
    up_begun++;
    up_len = l;
    up_got = up_sum = 0;
    return l >= 1024;
}
bool update_write(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        up_sum = up_sum * 31 + p[i];
    }
    up_got += n;
    return up_fail_at < 0 || up_got < (uint32_t)up_fail_at;
}
update_result_t update_finish(void)
{
    up_finished++;
    return up_result;
}
void update_abort(void) { up_aborted++; }
static uint8_t bs_file[32];
static bool bs_valid;
bool bootstate_read(uint8_t s[32])
{
    memcpy(s, bs_file, 32);
    return bs_valid;
}
bool bootstate_trial_pending(void) { return false; }
void bootstate_report(boot_check_t c, bool ok)
{
    if (c == BOOT_CHECK_TIME && ok)
    {
        boot_time_reports++;
    }
}
static int marks, update_round;
void monitor_mark(activity_t a)
{
    if (a == ACT_CLOUD)
    {
        marks++;
    }
}
void monitor_update_round(bool on) { update_round += on ? 1 : -1; }
static int box_busy = -1;
void monitor_sync_activity(bool b) { box_busy = b; }
static int n_fn, fn_any;
static uint16_t fn_func[16];
static uint8_t fn_var[16];
static log_args_t fn_args[16];
void log_arg_num(log_args_t *a, int32_t v)
{
    for (int i = 0; i < 4; i++)
    {
        a->data[a->n++] = (uint8_t)((uint32_t)v >> (8 * i));
    }
}
void log_arg_text(log_args_t *a, const char *t)
{
    size_t l = strlen(t);
    memcpy(a->data + a->n, t, l);
    a->n += (uint8_t)l;
}
void log_fn(uint8_t level, uint16_t group, uint16_t function, uint8_t variant, const log_args_t *a)
{
    if ((fn_any || (level == 1 && group == 22 && !variant)) && n_fn < 16)
    {
        fn_func[n_fn] = function;
        fn_var[n_fn] = variant;
        if (a)
        {
            fn_args[n_fn] = *a;
        }
        else
        {
            memset(&fn_args[n_fn], 0, sizeof fn_args[0]);
        }
        n_fn++;
    }
}
bool player_busy(void) { return false; }
uint32_t player_decode_ms;
void player_sync_result(int e, bool u)
{
    (void)u;
    sync_res = e;
}
void sound_limits_changed(void) { printf("    | sound limits changed\n"); }
uint8_t settings_language(void) { return 0; }
uint32_t settings_archive_audio_id(void) { return 1534781997; }
settings_t settings = {3, 3, 1, 1, 0, 0};
bool settings_ready(void) { return true; }
static uint8_t bb_sram[112];
const volatile buildblock_t *buildblock_in_sram(void) { return (const volatile buildblock_t *)bb_sram; }

/* --- scripted server --- */
static const char *answers[16];
static size_t answer_len[16];
static int n_answers, next_answer;
static char sent[16][2048];
static size_t sent_len[16];
static int n_sent;
static const char *cur;
static size_t cur_len, cur_pos;
static int chunk = 97, busy_left;
static bool yield_at_connect; /* the log connection was asked to yield when the connect succeeded */
bool cloud_log_yield_wanted(void);
static int socket_err;
int16_t sl_Socket(int16_t d, int16_t t, int16_t p)
{
    (void)d;
    (void)t;
    (void)p;
    if (socket_err)
    {
        return (int16_t)socket_err;
    }
    return 7;
}
int16_t sl_Close(int16_t s)
{
    (void)s;
    return 0;
}
int16_t sl_SetSockOpt(int16_t s, int16_t l, int16_t o, const void *v, SlSocklen_t n)
{
    (void)s;
    (void)l;
    if (o == SL_SO_SECURE_FILES_CA_FILE_NAME)
    {
        printf("    | CA option %.*s\n", (int)n, (const char *)v);
    }
    return 0;
}
int16_t sl_Connect(int16_t s, const SlSockAddr_t *a, int16_t l)
{
    (void)s;
    (void)a;
    (void)l;
    if (busy_left)
    {
        busy_left--;
        return SL_ESECT00MANYSSLOPENED;
    }
    yield_at_connect = cloud_log_yield_wanted();
    if (next_answer >= n_answers)
    {
        return -111;
    }
    cur = answers[next_answer];
    cur_len = answer_len[next_answer];
    next_answer++;
    cur_pos = 0;
    n_sent++;
    sent_len[n_sent - 1] = 0;
    return 0;
}
int16_t sl_Send(int16_t s, const void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)f;
    memcpy(sent[n_sent - 1] + sent_len[n_sent - 1], b, n);
    sent_len[n_sent - 1] += n;
    return n;
}
static int eagains;
static bool close_at_end; /* the server closes the connection after the answer */
int16_t sl_Recv(int16_t s, void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)f;
    recv_calls++;
    if (recv_hook)
    {
        recv_hook(recv_calls);
    }
    if (cur_pos >= cur_len && close_at_end)
    {
        return 0;
    }
    if (cur_pos >= cur_len)
    {
        sim_ms += 1000;
        if (++eagains > 100)
        {
            printf("    | (server silent too long)\n");
            exit(2);
        }
        return SL_EAGAIN;
    }
    size_t k = cur_len - cur_pos;
    if (k > (size_t)n)
    {
        k = n;
    }
    if (k > (size_t)chunk)
    {
        k = chunk;
    }
    memcpy(b, cur + cur_pos, k);
    cur_pos += k;
    return (int16_t)k;
}
static int dns_fail, dns_fail_once, dns_calls;
int16_t sl_NetAppDnsGetHostByName(signed char *h, uint16_t n, unsigned long *ip, uint8_t f)
{
    (void)h;
    (void)n;
    (void)f;
    dns_calls++;
    if (dns_fail_once)
    {
        dns_fail_once = 0;
        return -1;
    }
    if (dns_fail)
    {
        return -1;
    }
    *ip = 0x0A000001;
    return 0;
}
static int devset_fail;
int32_t sl_DevSet(uint8_t a, uint8_t b, uint8_t c, uint8_t *d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return devset_fail ? -1 : 0;
}
int32_t sl_NetCfgSet(uint8_t a, uint8_t b, uint8_t c, uint8_t *d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return 0;
}
/* flash files: name, content, length */
static struct
{
    const char *name;
    const uint8_t *data;
    unsigned long len;
} flash[4];
static int n_flash;
void page_run(uint8_t *w, unsigned wn, char *b, unsigned bn)
{
    (void)w;
    (void)wn;
    (void)b;
    (void)bn;
}
static int flash_find(const unsigned char *n)
{
    for (int i = 0; i < n_flash; i++)
    {
        if (!strcmp(flash[i].name, (const char *)n))
        {
            return i;
        }
    }
    return -1;
}
/* Writes go to wbuf (fs_write_fail: they fail; fs_write_bad: a byte is changed, the read-back differs). */
static uint8_t wbuf[4][4096];
static int fs_write_fail, fs_write_bad, fs_writes;
long sl_FsOpen(unsigned char *n, unsigned long m, unsigned long *t, long *fh)
{
    (void)t;
    int i = flash_find(n);
    if (i < 0 && m == FS_MODE_OPEN_CREATE(0, 0) && n_flash < 4)
    {
        i = n_flash++;
        flash[i].name = strdup((const char *)n);
    }
    if (i < 0)
    {
        return -11;
    }
    if (m == FS_MODE_OPEN_CREATE(0, 0))
    {
        flash[i].data = wbuf[i];
        flash[i].len = 0;
    }
    *fh = i;
    return 0;
}
long sl_FsRead(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    if (fh < 0 || fh >= n_flash || o + l > flash[fh].len)
    {
        return -1;
    }
    memcpy(b, flash[fh].data + o, l);
    return (long)l;
}
long sl_FsWrite(long fh, unsigned long o, unsigned char *b, unsigned long l)
{
    if (fs_write_fail || flash[fh].data != wbuf[fh])
    {
        return -1;
    }
    memcpy(wbuf[fh] + o, b, l);
    if (fs_write_bad)
    {
        wbuf[fh][0] ^= 1;
    }
    if (o + l > flash[fh].len)
    {
        flash[fh].len = o + l;
    }
    fs_writes++;
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
    int k = flash_find(n);
    if (k < 0)
    {
        return -11;
    }
    memset(i, 0, sizeof *i);
    i->FileLen = flash[k].len;
    return 0;
}

static void serve(const char *a, size_t len)
{
    answers[n_answers] = a;
    answer_len[n_answers++] = len ? len : strlen(a);
}
static void reset(void)
{
    n_answers = next_answer = n_sent = 0;
    eagains = 0;
    close_at_end = false;
}

/* --- FatFs over a host directory --- */
static char hidden[16][300];
static int n_hidden;
static void full(const char *p, char *o)
{
    snprintf(o, 600, "%s/%s", root, p[0] == '/' ? p + 1 : p);
    for (char *c = o + strlen(root); *c; c++)
    {
        *c = (char)toupper((unsigned char)*c);
    }
}
static const char *cardpath(const char *p)
{
    static char q[600];
    full(p, q);
    return q;
}
FRESULT f_open(FIL *f, const char *p, BYTE m)
{
    char q[600];
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
static int n_writes; /* f_write calls, for the 4096-byte collection of downloads */
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w)
{
    n_writes++;
    *w = fwrite(b, 1, n, f->fp);
    long p = ftell(f->fp);
    if ((FSIZE_t)p > f->size)
    {
        f->size = p;
    }
    return FR_OK;
}
static FSIZE_t lseek_limit; /* nonzero: a full card, files grow only to this size */
FRESULT f_lseek(FIL *f, FSIZE_t o)
{
    if (lseek_limit && o > lseek_limit)
    {
        o = lseek_limit;
    }
    if (o > f->size)
    {
        fseek(f->fp, o - 1, SEEK_SET);
        fputc(0, f->fp);
        f->size = o;
    }
    return fseek(f->fp, o, SEEK_SET) ? FR_DISK_ERR : FR_OK;
}
FRESULT f_sync(FIL *f)
{
    fflush(f->fp);
    return FR_OK;
}
FRESULT f_close(FIL *f)
{
    if (f->fp)
    {
        fclose(f->fp);
    }
    f->fp = NULL;
    return FR_OK;
}
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
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    for (int i = 0; i < n_hidden; i++)
    {
        if (!strcmp(hidden[i], q))
        {
            fi->fattrib |= AM_HID;
        }
    }
    return FR_OK;
}
FRESULT f_unlink(const char *p)
{
    char q[600];
    full(p, q);
    return (unlink(q) && rmdir(q)) ? FR_NO_FILE : FR_OK;
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
FRESULT f_chmod(const char *p, BYTE a, BYTE m)
{
    (void)m;
    char q[600];
    full(p, q);
    if (a & AM_HID)
    {
        strcpy(hidden[n_hidden++], q);
    }
    else
    {
        for (int i = 0; i < n_hidden; i++)
        {
            if (!strcmp(hidden[i], q))
            {
                hidden[i][0] = 0;
            }
        }
    }
    return FR_OK;
}
FRESULT f_opendir(DIR *d, const char *p)
{
    full(p, d->path);
    d->d = opendir(d->path);
    return d->d ? FR_OK : FR_NO_PATH;
}
FRESULT f_readdir(DIR *d, FILINFO *fi)
{
    struct dirent *e;
    memset(fi, 0, sizeof *fi);
    do
    {
        e = readdir(d->d);
    } while (e && e->d_name[0] == '.');
    if (!e)
    {
        return FR_OK;
    }
    snprintf(fi->fname, sizeof fi->fname, "%s", e->d_name);
    char q[900];
    snprintf(q, sizeof q, "%s/%s", d->path, e->d_name);
    struct stat st;
    stat(q, &st);
    fi->fsize = st.st_size;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    struct tm tm;
    time_t mt = st.st_mtime;
    gmtime_r(&mt, &tm);
    if (tm.tm_year >= 80)
    {
        fi->fdate = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
        fi->ftime = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    }
    return FR_OK;
}
FRESULT f_closedir(DIR *d)
{
    closedir(d->d);
    return FR_OK;
}
static FATFS host_fs = {64, 100000};
FRESULT f_getfree(const char *p, DWORD *n, FATFS **fs)
{
    (void)p;
    *n = 90000;
    *fs = &host_fs;
    return FR_OK;
}

static void show_request(int i) { printf("    > %.*s\n", (int)(strstr(sent[i], "\r\n\r\n") ? strstr(sent[i], "\r\n\r\n") - sent[i] : (long)sent_len[i]), sent[i]); }

/* --- teddyCloud extension --- */
static const uint8_t ex_profile[23] = {0x54, 0x42, 0x53, 0x31, 0x01, 0x01, 0x07, 0x00, 0x80, 0x3B, 0xB1, 0x6A, 0x01, 0x01, 0x01, 0x03, 0x02, 0x1E, 0x00, 0xF7, 0xB8, 0x55, 0x2C};
static const uint8_t ex_global[19] = {0x54, 0x42, 0x53, 0x31, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x01, 0x02, 0x88, 0x71, 0xBD, 0x37};

/* a TBS file of kind k, stamp t and the given body, CRC computed */
static unsigned mk_tbs(uint8_t *f, uint8_t k, uint32_t t, const uint8_t *body, unsigned n)
{
    memcpy(f, "TBS1", 4);
    f[4] = k;
    f[5] = 1;
    f[6] = (uint8_t)n;
    f[7] = (uint8_t)(n >> 8);
    ext_put32(f + 8, t);
    memcpy(f + 12, body, n);
    ext_put32(f + 12 + n, crc32_ogg(0, f, 12 + n));
    return 16 + n;
}

/* field 100 with uid and file appended to m */
static unsigned put100(uint8_t *m, const uint8_t uid[8], const uint8_t *f, unsigned n)
{
    unsigned k = 0;
    m[k++] = 0xA2;
    m[k++] = 0x06;
    m[k++] = (uint8_t)(8 + n);
    memcpy(m + k, uid, 8);
    memcpy(m + k + 8, f, n);
    return k + 8 + n;
}

static bool card_file(const char *path, uint8_t *buf, unsigned *n)
{
    FIL f;
    UINT g = 0;
    if (f_open(&f, path, FA_READ) != FR_OK)
    {
        return false;
    }
    f_read(&f, buf, 4096, &g);
    f_close(&f);
    *n = g;
    return true;
}

static void ext_tests(const uint8_t *taf, long tz)
{
    (void)taf;
    (void)tz;
    printf("X1 TBS CRC and the worked examples\n");
    CHECK(crc32_ogg(0, "123456789", 9) == 0x89A1897Fu, "check value");
    CHECK(ext_tbs_valid(ex_profile, 23) && ext_tbs_valid(ex_global, 19), "examples valid");
    ext_settings_t st;
    ext_parse_settings(ex_profile, &st);
    CHECK(st.kind == 1 && ext_has(&st, EXT_RESUME) && st.value[EXT_RESUME] == 1 && st.value[EXT_SLEEP] == 30 && st.present == (1 << 1 | 1 << 3) && st.stamp == 1790000000u, "profile: resume 1, sleep 30");
    ext_parse_settings(ex_global, &st);
    CHECK(st.kind == 3 && st.present == 1 << 6 && st.value[EXT_LED] == 2, "global: LED 2");
    printf("X2 TBS checks\n");
    {
        uint8_t f[300], b[260] = {0};
        unsigned n;
        n = mk_tbs(f, 1, 5, b, 241);
        CHECK(n == 257 && !ext_tbs_valid(f, n), "257 bytes refused");
        static const uint8_t body[] = {0x01, 0x01, 0x01};
        n = mk_tbs(f, 1, 5, body, 3);
        CHECK(ext_tbs_valid(f, n), "valid");
        f[0] = 'X';
        CHECK(!ext_tbs_valid(f, n), "magic");
        f[0] = 'T';
        for (int k = 0; k <= 4; k += 4)
        {
            n = mk_tbs(f, (uint8_t)k, 5, body, 3);
            CHECK(!ext_tbs_valid(f, n), "kind %d", k);
        }
        n = mk_tbs(f, 1, 5, body, 3);
        f[6] = 4;
        CHECK(!ext_tbs_valid(f, n), "body length one too large");
        f[6] = 2;
        CHECK(!ext_tbs_valid(f, n), "one too small");
        f[6] = 3;
        f[n - 1] ^= 1;
        CHECK(!ext_tbs_valid(f, n), "CRC byte");
        static const uint8_t b5[] = {0x55, 3, 1, 2, 3, 0x01, 2, 1, 1, 0x04, 1, 16, 0x07, 1, 0, 0x01, 1, 1};
        n = mk_tbs(f, 1, 5, b5, sizeof b5);
        ext_parse_settings(f, &st);
        CHECK(ext_tbs_valid(f, n) && st.present == 1 << 1 && st.value[EXT_RESUME] == 1, "unknown key, wrong length, out of range, 0x07 in a profile skipped");
        static const uint8_t b6[] = {0x01, 2, 1};
        n = mk_tbs(f, 1, 5, b6, sizeof b6);
        CHECK(!ext_tbs_valid(f, n), "record past the end");
        static const uint8_t b7[] = {0x01, 1, 1, 0x02};
        n = mk_tbs(f, 1, 5, b7, sizeof b7);
        CHECK(!ext_tbs_valid(f, n), "one byte left over");
        static const uint8_t b8[] = {0x01, 1, 1, 0x04, 1, 5, 0x07, 1, 0};
        n = mk_tbs(f, 3, 5, b8, sizeof b8);
        ext_parse_settings(f, &st);
        CHECK(st.present == (1 << 4 | 1 << 7) && st.value[EXT_BOTH_EARS] == 0 && st.value[EXT_SPEAKER_LIMIT] == 5, "global: 0x01 skipped, 0x04 and 0x07 kept");
    }

    printf("X3 event tag file\n");
    {
        uint8_t f[100];
        static const uint8_t b[] = {0x10, 3, 1, 0x00, 0x40, 0x10, 3, 8, 3, 0x00, 0x10, 2, 9, 9, 0x11, 1, 0, 0x10, 3, 3, 20, 0x00};
        unsigned n = mk_tbs(f, 2, 7, b, sizeof b);
        ext_event_t e;
        ext_parse_event(f, &e);
        CHECK(ext_tbs_valid(f, n) && e.n == 3 && e.a[0].action == 1 && e.a[0].mode == EXT_ON && e.a[1].action == 8 && e.a[1].value == 3 && e.a[2].value == 20 && !e.tones, "actions, modes, values, tones off");
        CHECK(ext_event_scope(&e) == EXT_SCOPE_FIGURE, "scope of the first action (1: figure)");
        e.a[0].action = 0;
        CHECK(ext_event_scope(&e) == EXT_SCOPE_GLOBAL, "action 0 has no scope, then 8: global");
    }

    printf("X4 shuffle check values\n");
    {
        uint8_t o[99];
        char t[64];
        static const struct
        {
            uint32_t s;
            unsigned n;
            const char *want;
        } cv[] = {{1, 2, "1 0"}, {1, 5, "3 4 1 0 2"}, {1, 8, "5 6 2 0 4 7 1 3"}, {0x12345678, 2, "0 1"}, {0x12345678, 5, "0 4 1 3 2"}, {0x12345678, 8, "1 7 6 0 2 3 5 4"}};
        for (unsigned i = 0; i < 6; i++)
        {
            ext_shuffle(cv[i].s, cv[i].n, o);
            t[0] = 0;
            for (unsigned k = 0; k < cv[i].n; k++)
            {
                sprintf(t + strlen(t), k ? " %u" : "%u", o[k]);
            }
            CHECK(!strcmp(t, cv[i].want), "seed %#x n %u: %s", cv[i].s, cv[i].n, t);
        }
        ext_shuffle(77, 99, o);
        unsigned seen[99] = {0}, ok = 1;
        for (int k = 0; k < 99; k++)
        {
            seen[o[k]]++;
        }
        for (int k = 0; k < 99; k++)
        {
            ok &= seen[k] == 1;
        }
        CHECK(ok, "99 chapters: a permutation");
    }

    printf("X5 state record example and the state file\n");
    {
        static const uint8_t want[32] = {0x47, 0x3F, 0x0D, 0x0D, 0x50, 0x03, 0x04, 0xE0, 0x05, 0, 0, 0, 0x80, 0x3B, 0xB1, 0x6A, 0x78, 0, 0, 0, 0x78, 0x56, 0x34, 0x12, 0x03, 0x08, 0x01, 0, 0, 0, 0, 0x75};
        ext_state_t s = {{0x47, 0x3F, 0x0D, 0x0D, 0x50, 0x03, 0x04, 0xE0}, 5, 0x6AB13B80, 120, 0x12345678, 3, 8, 1}, g;
        uint8_t r[32];
        ext_state_pack(&s, r);
        CHECK(!memcmp(r, want, 32), "example bytes");
        CHECK(ext_state_unpack(r, &g) && g.position == 120 && g.seed == 0x12345678, "unpacked");
        r[5] ^= 1;
        CHECK(!ext_state_unpack(r, &g), "wrong check: free");
        CHECK(!ext_state_get(s.uid, &g), "no file: no record");
        CHECK(ext_state_put(&s) && s.sequence == 1, "first write");
        uint8_t buf[4096];
        unsigned n;
        CHECK(card_file(STATE_PATH, buf, &n) && n == 2048 && !memcmp(buf, s.uid, 8), "2048 bytes, record 0");
        s.position = 200;
        ext_state_put(&s);
        CHECK(ext_state_get(s.uid, &g) && g.position == 200 && g.sequence == 2, "rewritten in place (sequence %u)", g.sequence);
        card_file(STATE_PATH, buf, &n);
        CHECK(buf[32 + 8] == 0, "record 1 still free");
        for (int i = 0; i < 63; i++)
        {
            ext_state_t o = {{1, 2, 3, (uint8_t)i, 5, 6, 7, 0xE0}};
            ext_state_put(&o);
        }
        ext_state_t nw = {{9, 9, 9, 9, 9, 9, 9, 0xE0}, 0, 1, 2};
        ext_state_put(&nw);
        CHECK(!ext_state_get(s.uid, &g) && ext_state_get(nw.uid, &g) && g.sequence == 66, "full table: the lowest sequence replaced");
    }

    printf("X6 freshness answer with hello and settings files\n");
    {
        static const uint8_t fig[8] = {0x47, 0x3F, 0x0D, 0x0D, 0x50, 0x03, 0x04, 0xE0}, zero[8] = {0};
        uint8_t m[1024], f[64], bad[64];
        unsigned k = 0, n;
        n_fn = 0;
        m[k++] = 0x18;
        m[k++] = 0x03; /* field 3 */
        k += put100(m + k, fig, ex_profile, 23);
        memcpy(bad, ex_global, 19);
        bad[17] ^= 1;
        k += put100(m + k, zero, bad, 19);      /* CRC broken */
        k += put100(m + k, fig, ex_global, 19); /* global under a figure */
        m[k++] = 0xA8;
        m[k++] = 0x06;
        m[k++] = 0x01; /* field 101 */
        k += put100(m + k, zero, ex_global, 19);
        m[k++] = 0xA2;
        m[k++] = 0x06;
        m[k++] = 8;
        memcpy(m + k, fig, 8);
        k += 8; /* 8 bytes */
        static fresh_t r;
        CHECK(fresh_decode(m, k, &r), "answer decodes");
        reset();
        static char ans[1200];
        int hl = sprintf(ans, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", k);
        memcpy(ans + hl, m, k);
        serve(ans, hl + k);
        settings.limit_speaker = 1;
        cloud_error_t e = fresh_session(1);
        uint8_t buf[4096];
        CHECK(e == CE_OK && card_file("CFW/EXT/473F0D0D/500304E0.TBS", buf, &n) && n == 23 && !memcmp(buf, ex_profile, 23), "profile stored byte for byte");
        CHECK(card_file("CFW/EXT/GLOBAL.TBS", buf, &n) && n == 19 && !memcmp(buf, ex_global, 19), "global stored (broken one and the one under a figure ignored)");
        CHECK(n_fn == 1 && fn_func[0] == 32257 && fn_args[0].n == 16 && ext_le32(fn_args[0].data) == CFW_BUILD_TIME && ext_le32(fn_args[0].data + 4) == EXT_CAPS, "one hello answer record: version, capabilities");
        CHECK(settings.limit_speaker == 3, "field 3 still applied");
        ext_answer(m, k);
        CHECK(n_fn == 1, "second hello: no record");

        printf("    hello in an unconfirmed trial start: answered once the trial is confirmed\n");
        hello = false;
        n_fn = 0;
        ext_set_boot_result(0, 0, true);
        ext_answer(m, k);
        ext_hello_flush();
        CHECK(n_fn == 0, "no hello answer while the trial start is not confirmed");
        ext_trial_confirmed();
        ext_set_boot_result(0, 0, false); /* a late read of the old state file does not undo it */
        ext_hello_flush();
        CHECK(n_fn == 1 && fn_func[0] == 32257 && ext_le32(fn_args[0].data + 8) == 1, "then one hello answer with last result 1");
        ext_hello_flush();
        CHECK(n_fn == 1, "only one");
        trial_confirmed = false;
        boot_result = 0;
        n_fn = 1;
        static const uint8_t b2[] = {0x02, 1, 1};
        n = mk_tbs(f, 1, 1790000000u - 1, b2, 3);
        k = put100(m, fig, f, n);
        ext_answer(m, k);
        card_file("CFW/EXT/473F0D0D/500304E0.TBS", buf, &n);
        CHECK(n == 23, "1 s older: kept");
        n = mk_tbs(f, 1, 1790000000u, b2, 3);
        k = put100(m, fig, f, n);
        ext_answer(m, k);
        card_file("CFW/EXT/473F0D0D/500304E0.TBS", buf, &n);
        CHECK(n == 23, "same time stamp: kept");
        n = mk_tbs(f, 1, 1790000001u, b2, 3);
        k = put100(m, fig, f, n);
        uint32_t g0 = ext_generation();
        ext_answer(m, k);
        card_file("CFW/EXT/473F0D0D/500304E0.TBS", buf, &n);
        CHECK(n == 19 && ext_generation() != g0, "1 s newer: stored");
        ext_settings_t p;
        CHECK(ext_profile(fig, &p) && ext_has(&p, EXT_SHUFFLE) && !ext_has(&p, EXT_RESUME), "ext_profile reads it");
        static const uint8_t ev[] = {0x10, 3, 1, 0, 0};
        static const uint8_t evu[8] = {0x12, 0x34, 0x56, 0x78, 0x50, 0x03, 0x04, 0xE0};
        n = mk_tbs(f, 2, 3, ev, sizeof ev);
        k = put100(m, evu, f, n);
        ext_answer(m, k);
        ext_event_t ee;
        CHECK(ext_event_tag(evu, &ee) && ee.n == 1 && !ext_profile(evu, &p), "event tag file stored; not a profile");
        ext_settings_t gl;
        CHECK(ext_global(&gl) && gl.value[EXT_LED] == 2, "ext_global");
        static const uint8_t unk[8] = {0x77, 0x34, 0x56, 0x78, 0x50, 0x03, 0x04, 0xE0};
        CHECK(!ext_profile(unk, &p), "no file: absent");
    }

    printf("X7 the box writes a profile and reports it\n");
    {
        static const uint8_t fig2[8] = {0x01, 0x02, 0x03, 0x04, 0x50, 0x03, 0x04, 0xE0};
        ext_settings_t s = {0};
        ext_set(&s, EXT_RESUME, 1);
        n_fn = 0;
        uint8_t buf[4096];
        unsigned n;
        CHECK(ext_write(fig2, &s) && card_file("CFW/EXT/01020304/500304E0.TBS", buf, &n) && n == 19 && buf[4] == 1 && buf[5] == 0 && buf[12] == 1 && buf[13] == 1 && buf[14] == 1 && ext_tbs_valid(buf, n) && ext_le32(buf + 8) == clock_now(), "19 bytes, kind 1, origin 0, body 01 01 01, clock time stamp");
        CHECK(n_fn == 1 && fn_func[0] == 32258 && fn_args[0].n == 20 && !memcmp(fn_args[0].data, fig2, 8) && ext_le32(fn_args[0].data + 8) == (1u | 1u << 4 | 1u << 12) && ext_le32(fn_args[0].data + 16) == ext_le32(buf + 8), "report: UID bytes, kind 1, presence bit 4, resume bit 12, time stamp");
        ext_settings_t s2 = {0};
        ext_set(&s2, EXT_SHUFFLE, 1);
        ext_set(&s2, EXT_SLEEP, 300);
        ext_set(&s2, EXT_HEADPHONE_LIMIT, 9);
        ext_set(&s2, EXT_TILT_LOCK, 1);
        ext_set(&s2, EXT_SLAP_LOCK, 0);
        ext_set(&s2, EXT_BOTH_EARS, 1);
        ext_write(fig2, &s2);
        card_file("CFW/EXT/01020304/500304E0.TBS", buf, &n);
        static const uint8_t body[] = {0x02, 1, 1, 0x03, 2, 0x2C, 0x01, 0x05, 1, 9, 0x08, 1, 0, 0x09, 1, 1};
        CHECK(n == 16 + sizeof body && !memcmp(buf + 12, body, sizeof body), "ascending keys, 2-byte sleep, 0x07 left out of a profile");
        uint32_t pk, vl;
        ext_pack_report(&s2, &pk, &vl);
        CHECK(pk == (1u | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 10 | 1u << 13 | 1u << 16) && vl == (300u | 9u << 20 | 1u << 24 | 1u << 25 | 1u << 28), "packed %#x values %#x", pk, vl);
        ext_settings_t g = {0};
        ext_set(&g, EXT_LED, 1);
        n_fn = 0;
        ext_write(NULL, &g);
        CHECK(n_fn == 1 && ext_le32(fn_args[0].data) == 0 && ext_le32(fn_args[0].data + 4) == 0 && (ext_le32(fn_args[0].data + 8) & 3) == 3, "global: UID 0, kind 3");
        hello = false;
        n_fn = 0;
        ext_write(fig2, &s);
        CHECK(n_fn == 0, "no report before the hello");
    }
    printf("X9 ext_store for remote command 64\n");
    {
        static const uint8_t u[8] = {0x31, 0x32, 0x33, 0x34, 0x50, 0x03, 0x04, 0xE0}, zero8[8] = {0};
        uint8_t m[300], f[64];
        static const uint8_t b[] = {0x01, 1, 1};
        unsigned n = mk_tbs(f, 1, 100, b, 3);
        memcpy(m, u, 8);
        memcpy(m + 8, f, n);
        f_unlink("CFW/EXT/31323334/500304E0.TBS");
        CHECK(!ext_store(m, 8), "8 bytes: false");
        CHECK(ext_store(m, 8 + n), "valid profile: true");
        CHECK(!ext_store(m, 8 + n), "same time stamp: false");
        memcpy(m, zero8, 8);
        CHECK(!ext_store(m, 8 + n), "profile under UID 0: false");
        memcpy(m, u, 8);
        m[8 + n - 1] ^= 1;
        CHECK(!ext_store(m, 8 + n), "CRC broken: false");
        n = mk_tbs(f, 1, 101, b, 3);
        memcpy(m + 8, f, n);
        CHECK(ext_store(m, 8 + n), "newer: true");
    }
    printf("X8 boot result in the hello answer\n");
    {
        bs_valid = true;
        bs_file[9] = 2;
        bs_file[12] = 0x80;
        bs_file[13] = 0x3B;
        bs_file[14] = 0xB1;
        bs_file[15] = 0x6A;
        boot_state_read();
        uint8_t m[3] = {0xA8, 0x06, 0x01};
        hello = false;
        n_fn = 0;
        ext_answer(m, 3);
        CHECK(n_fn == 1 && ext_le32(fn_args[0].data + 8) == 2 && ext_le32(fn_args[0].data + 12) == 0x6AB13B80u, "last result 2, rolled-back build");
    }
}

#ifdef CFW_ESP32
/* --- ESP32 box: what reaches the vendor's cloud equals the
 * original ESP firmware's requests --- */
static char time_server[66];
static int time_syncs;
void net_time_sync(const char *server)
{
    snprintf(time_server, sizeof time_server, "%s", server);
    time_syncs++;
}
const char *update_original_agent(void) { return "toniebox-esp32-eu/v5.233.0"; }

static bool sent_any(const char *what)
{
    for (int i = 0; i < n_sent; i++)
    {
        if (strstr(sent[i], what))
        {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    root = argv[1];
    cloud_init();
    mkdir(cardpath("/cfw"), 0755);
    char q[600];
    full("cfw/config.txt", q);

    printf("E1 configuration: time server by default, from the ntp line, vendor host refused\n");
    FILE *w = fopen(q, "wb");
    fputs("host=192.0.2.10\n", w);
    fclose(w);
    config_read();
    CHECK(cfg.valid && !strcmp(cfg.ntp, "pool.ntp.org"), "default pool.ntp.org");
    w = fopen(q, "wb");
    fputs("host=192.0.2.10\nntp=192.0.2.1\n", w);
    fclose(w);
    config_read();
    CHECK(!strcmp(cfg.ntp, "192.0.2.1"), "IP address taken");
    w = fopen(q, "wb");
    fputs("ntp=time.tbs.toys\n", w);
    fclose(w);
    config_read();
    CHECK(!cfg.valid && !strcmp(cfg.ntp, "pool.ntp.org"), "vendor host refused, no server line: no server");

    printf("E2 online round without a server: time server started, no request\n");
    reset();
    online_round(1);
    CHECK(time_syncs == 1 && !strcmp(time_server, "pool.ntp.org") && n_sent == 0 && first_result == CE_NAME,
          "time from the time server, round fails without a server");

    printf("E3 online round: the freshness check is the first exchange, no time request, no update check\n");
    w = fopen(q, "wb");
    fputs("host=192.0.2.10\n", w);
    fclose(w);
    config_read();
    reset();
    update_checked = false;
    boot_time_reports = 0;
    static const uint8_t body[] = {0x40, 0x02};
    static char ans[200];
    int hl = sprintf(ans, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", sizeof body);
    memcpy(ans + hl, body, sizeof body);
    serve(ans, hl + sizeof body);
    online_round(1);
    show_request(0);
    static const char head[] = "POST /v1/freshness-check HTTP/1.1\r\nHost: 192.0.2.10\r\nUser-Agent: toniebox-esp32-eu/v5.233.0\r\n";
    CHECK(n_sent == 1 && !strncmp(sent[0], head, strlen(head)),
          "one request: the freshness check with the original user agent");
    CHECK(!sent_any("/v1/time") && !sent_any("/v1/ota/") && !update_checked, "no time request, no update check");
    CHECK(first_result == CE_OK && state == 6 && boot_time_reports == 1, "first result, state 6, trial start confirmed");

    printf("E4 no claim for a complete content\n");
    reset();
    unsigned waiting = uxQueueMessagesWaiting(queue);
    uint8_t uid[8] = {0xE0, 0x04, 0x03, 0x50, 0x0E, 0xEE, 0x06, 0xB7}, token[32] = {0};
    cloud_claim(uid, token);
    CHECK(uxQueueMessagesWaiting(queue) == waiting, "nothing queued");

    printf("E5 no log post\n");
    reset();
    outcome_new = true;
    outcome_state = 3;
    outcome_code = -1;
    log_post();
    CHECK(n_sent == 0, "nothing sent");

    printf("E6 update check and setup page only after the hello of an extension server\n");
    reset();
    hello = false;
    update_checked = page_checked = false;
    update_after_hello();
    CHECK(n_sent == 0, "no hello: no request");
    hello = true;
    serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    update_after_hello();
    update_after_hello();
    char want[64];
    sprintf(want, "GET /v1/ota/3?cv=%u ", CFW_BUILD_TIME);
    CHECK(n_sent == 2 && !strncmp(sent[0], want, strlen(want)) && strstr(sent[1], "GET /v1/ota/7?cv=") == sent[1],
          "item 3 with the own build number, then item 7, once per start");

    unlink(q);
    full("cfw", q);
    rmdir(q);
    printf("\n%d failure(s)\n", failures);
    return failures != 0;
}
#else
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    root = argv[1];
    cloud_init();
    memset(&cfg, 0, sizeof cfg);
    cfg.valid = true;
    cfg.is_ip = true;
    cfg.ip = 0xC0A8057A;
    strcpy(cfg.host, "192.0.2.10");
    cfg.port = 443;
    strcpy(cfg.ca, "/cert/c2.der");

    printf("T1 time request\n");
    reset();
    serve("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n1791776107", 0);
    bool ok = time_request(1);
    show_request(0);
    CHECK(ok && clock_now() >= 1791776107, "clock set");
    static const char want[] = "GET /v1/time HTTP/1.1\r\nHost: 192.0.2.10\r\nUser-Agent: TB/1779199517 SP/";
    CHECK(!strncmp(sent[0], want, strlen(want)), "request line, Host, User-Agent of 3.4.0");
    {
        char hdr[48];
        sprintf(hdr, "\r\nX-CFW-Build: %u\r\n\r\n", CFW_BUILD_TIME);
        CHECK(strstr(sent[0], hdr) != NULL, "own build number in X-CFW-Build, last header");
    }

    CHECK(boot_time_reports == 1, "time check reported to the boot state");
    printf("T2 time with a surplus byte in the same segment\n");
    reset();
    serve("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n17917761075", 0);
    ok = time_request(1);
    CHECK(ok && clock_now() < 1791776200, "surplus byte ignored (clock %u)", clock_now());

    CHECK(boot_time_reports == 1, "reported once per start");
    printf("T3 header block above 508 bytes\n");
    reset();
    static char big[700];
    strcpy(big, "HTTP/1.1 200 OK\r\nX: ");
    memset(big + 20, 'a', 520);
    strcpy(big + 540, "\r\nContent-Length: 10\r\n\r\n1791776107");
    serve(big, 0);
    ok = time_request(1);
    CHECK(!ok, "rejected");

    printf("T4 update check under the SD bootloader, items answered 304\n");
    reset();
    update_checked = false;
    for (int i = 0; i < 5; i++)
    {
        serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    }
    update_check(1);
    for (int i = 0; i < n_sent; i++)
    {
        show_request(i);
    }
    CHECK(n_sent == 5 && strstr(sent[0], "/v1/ota/4?cv=34471936") && strstr(sent[1], "/v1/ota/5?cv=1 ") && strstr(sent[2], "/v1/ota/2?cv=") && strstr(sent[3], "/v1/ota/6?cv=1534781997") && strstr(sent[4], "/v1/ota/3?cv=1779199517 "), "order 4 5 2 6 3, item 3 with the build number of 3.4.0");
    printf("T4b update check under the original bootloader: the same items, item 3 offered is abandoned\n");
    reset();
    update_checked = false;
    ho.valid = false;
    up_begun = 0;
    for (int i = 0; i < 4; i++)
    {
        serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    }
    serve("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\nxxxx", 0);
    update_check(1);
    ho.valid = true;
    CHECK(n_sent == 5 && strstr(sent[4], "/v1/ota/3?") && !up_begun, "items 4 5 2 6 3, nothing installed (%d requests)", n_sent);
    printf("T4c no free slot: item 3 asked for, an offer abandoned\n");
    reset();
    update_checked = false;
    free_slot = 0;
    up_begun = 0;
    for (int i = 0; i < 4; i++)
    {
        serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    }
    serve("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\nxxxx", 0);
    update_check(1);
    free_slot = 2;
    CHECK(n_sent == 5 && !up_begun, "item 3 asked for, not installed (%d requests)", n_sent);
    printf("T4f item 2 answered 200: abandoned, item 6 follows\n");
    reset();
    update_checked = false;
    for (int i = 0; i < 2; i++)
    {
        serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    }
    serve("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\nxxxx", 0);
    serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    update_check(1);
    CHECK(n_sent == 5 && strstr(sent[3], "/v1/ota/6?"), "items 6 and 3 follow (%d requests)", n_sent);
    printf("T4e item 7 only after a hello, then the setup page written to the card\n");
    mkdir(cardpath("/cfw"), 0755);
    reset();
    hello = false;
    page_checked = false;
    page_check(1);
    CHECK(n_sent == 0, "no hello: no item 7");
    hello = true;
    serve("HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\n<html>x</html", 0);
    page_check(1);
    page_check(1);
    {
        char page[64] = {0};
        FILE *pf = fopen(cardpath("/cfw/setup.htm"), "rb");
        size_t pn = pf ? fread(page, 1, sizeof page - 1, pf) : 0;
        if (pf)
        {
            fclose(pf);
        }
        CHECK(n_sent == 1 && strstr(sent[0], "/v1/ota/7?cv=0 ") && pn == 13 && !strcmp(page, "<html>x</html"),
              "once per start, cv 0 without a page, page of 13 bytes on the card");
        reset();
        page_checked = false;
        serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
        page_check(1);
        char want7[40];
        sprintf(want7, "/v1/ota/7?cv=%u ", crc32_ogg(0, "<html>x</html", 13) >> 1);
        CHECK(strstr(sent[0], want7) != NULL, "next start reports the page's CRC-32 >> 1 (%s)", sent[0]);
        remove(cardpath("/cfw/setup.htm"));
    }
    hello = false;
    page_checked = false;
    printf("T4d item 3 answered 200: the image streamed into the update module\n");
    {
        static char img[70000];
        int hl = sprintf(img, "HTTP/1.1 200 OK\r\nContent-Length: 60000\r\n\r\n");
        uint32_t want = 0;
        for (int i = 0; i < 60000; i++)
        {
            img[hl + i] = (char)(i * 7 + i / 300);
            want = want * 31 + (uint8_t)img[hl + i];
        }
        memcpy(img + hl + 60000, "SURPLUS", 7);
        reset();
        update_checked = false;
        up_begun = up_aborted = up_finished = 0;
        marks = 0;
        for (int i = 0; i < 4; i++) /* items 4, 5, 2, 6 */
        {
            serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
        }
        serve(img, hl + 60007);
        serve("HTTP/1.1 304 x\r\n\r\n", 0);
        update_check(1);
        CHECK(n_sent == 5 && up_begun == 1 && up_len == 60000 && up_got == 60000 && up_sum == want && up_finished == 1 && !up_aborted, "60000 bytes in order, surplus ignored, finished");
        CHECK(marks > 0, "marked while receiving");
        reset();
        update_checked = false;
        up_begun = up_aborted = up_finished = 0;
        up_fail_at = 30000;
        for (int i = 0; i < 4; i++) /* items 4, 5, 2, 6 */
        {
            serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
        }
        serve(img, hl + 60000);
        update_check(1);
        up_fail_at = -1;
        CHECK(up_aborted == 1 && !up_finished, "write refused: aborted, not finished");
        reset();
        update_checked = false;
        up_begun = up_aborted = up_finished = 0;
        static char cut[1000];
        int cl = sprintf(cut, "HTTP/1.1 200 OK\r\nContent-Length: 60000\r\n\r\n");
        memset(cut + cl, 1, 500);
        for (int i = 0; i < 4; i++) /* items 4, 5, 2, 6 */
        {
            serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
        }
        serve(cut, cl + 500);
        update_check(1);
        CHECK(up_aborted == 1 && !up_finished, "body stops (deadline 60 s): aborted");
        reset();
        update_checked = false;
        up_begun = up_aborted = 0;
        for (int i = 0; i < 4; i++) /* items 4, 5, 2, 6 */
        {
            serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
        }
        serve("HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n", 0);
        update_check(1);
        CHECK(!up_begun && n_sent == 5, "malformed length: refused before anything");
        reset();
        update_checked = false;
        up_begun = 0;
        serve("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", 0);
        update_check(1);
        CHECK(!up_begun && n_sent == 1, "200 for item 4: abandoned, no further item");
    }

    printf("T5 update check, item 5 answered 404, item 6 503\n");
    reset();
    update_checked = false;
    serve("HTTP/1.1 304 Not Modified\r\n\r\n", 0);
    serve("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0);
    serve("HTTP/1.1 503 x\r\n\r\n", 0);
    update_check(1);
    CHECK(n_sent == 3, "stops after 503 (%d requests)", n_sent);
    printf("T5b progress marks: silent server 5 s, busy connects\n");
    reset();
    marks = 0;
    busy_left = 3;
    serve("", 0);
    answer_len[0] = 0;
    recv_calls = 0;
    {
        void hook(int n)
        {
            if (n == 5)
            {
                dl_cancel = true;
            }
        }
        conn_t c;
        recv_hook = hook;
        dl_cancel = false;
        uint32_t t0 = sim_ms;
        exchange(&c, 1, 0, &dl_cancel, "GET", "/m", NULL, NULL, 0);
        recv_hook = NULL;
        close_conn(&c);
        dl_cancel = false;
        CHECK(marks >= (int)((sim_ms - t0) / 1000), "%d marks in %u ms", marks, sim_ms - t0);
    }
    printf("T5c online round: update round marked\n");
    reset();
    update_checked = false;
    update_round = 0;
    serve("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n1791776107", 0);
    serve("HTTP/1.1 503 x\r\n\r\n", 0);
    online_round(1);
    CHECK(update_checked && update_round == 0 && n_sent == 2, "update round begun and ended");

    printf("T6 freshness check: limit 1, LED dimmed, slap off, one mark\n");
    reset();
    static const uint8_t body[] = {0x0A, 0x08, 0xB7, 0x06, 0xEE, 0x0E, 0x50, 0x03, 0x04, 0xE0, 0x18, 0x01, 0x20, 0x00, 0x40, 0x02};
    static char ans[200];
    int hl = sprintf(ans, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", sizeof body);
    memcpy(ans + hl, body, sizeof body);
    serve(ans, hl + sizeof body);
    cloud_error_t e = fresh_session(1);
    show_request(0);
    const char *b = strstr(sent[0], "\r\n\r\n") + 4;
    size_t blen = sent_len[0] - (b - sent[0]);
    printf("    body %zu bytes:", blen);
    for (size_t i = 0; i < blen && i < 48; i++)
    {
        printf(" %02x", (uint8_t)b[i]);
    }
    printf("\n");
    CHECK(e == CE_OK, "session ok");
    CHECK(strstr(sent[0], "POST /v1/freshness-check HTTP/1.1\r\nHost: 192.0.2.10\r\nUser-Agent:") && strstr(sent[0], "Content-Length: "), "POST with Content-Length");
    CHECK(settings.limit_speaker == 1 && settings.led == 2 && settings.slap_enable == 0, "settings applied");
    CHECK(blen % 16 == 0 && (uint8_t)b[0] == 0x0A && (uint8_t)b[1] == 14 && (uint8_t)b[2] == 0x09, "entries 16 bytes: field 1, fixed64 uid");
    FILINFO fi;
    CHECK(f_stat("CONTENT/B706EE0E/500304E0", &fi) == FR_OK && (fi.fattrib & AM_HID), "figure marked outdated");

    printf("T7 content download, 200, whole file\n");
    reset();
    FILE *src = fopen(argv[2], "rb");
    fseek(src, 0, SEEK_END);
    long z = ftell(src);
    rewind(src);
    char *file = malloc(z + 200);
    int h7 = sprintf(file, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", z);
    fread(file + h7, 1, z, src);
    fclose(src);
    serve(file, h7 + z);
    chunk = 1400;
    static const uint8_t uid[8] = {0x11, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0}, tok[32] = {1, 2, 3};
    cloud_content(uid, tok, 0, 0);
    job_t j;
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    n_writes = 0;
    content_download(&j);
    show_request(0);
    CHECK(cloud_dl.done && cloud_dl.pos == (uint32_t)z && cloud_dl.readable == (uint32_t)z, "complete %u of %ld", cloud_dl.pos, z);
    CHECK(n_writes == (int)((z + 4095) / 4096), "written in 4096-byte pieces (%d writes)", n_writes);
    CHECK(strstr(sent[0], "GET /v2/content/11223344500304e0 HTTP/1.1") && strstr(sent[0], "Authorization: BD 0102030000"), "v2 path and token");
    char q[600];
    full("CONTENT/11223344/500304E0", q);
    FILE *got = fopen(q, "rb");
    fseek(got, 0, SEEK_END);
    CHECK(got && ftell(got) == z, "file size");
    rewind(got);
    char *gb = malloc(z);
    fread(gb, 1, z, got);
    fclose(got);
    CHECK(!memcmp(gb, file + h7, z), "file content equal");

    long off = (z / 2) & ~4095L;
    printf("T8 resumed download, 206 from %ld, after two busy connects\n", off);
    reset();
    busy_left = 2;
    char *f2 = malloc(z);
    int h8 = sprintf(f2, "HTTP/1.1 206 Partial\r\nContent-Length: %ld\r\nContent-Range: bytes %ld-%ld/%ld\r\n\r\n", z - off, off, z - 1, z);
    memcpy(f2 + h8, file + h7 + off, z - off);
    serve(f2, h8 + z - off);
    cloud_content(uid, NULL, off, 0xC0FFE1);
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    content_download(&j);
    show_request(0);
    char rng[64];
    sprintf(rng, "Range: bytes=%ld-\r\nIf-Range: 12648417\r\n", off);
    CHECK(cloud_dl.done && strstr(sent[0], "GET /v1/content/11223344500304e0") && strstr(sent[0], rng), "v1 with Range and If-Range");
    CHECK(yield_at_connect && !cloud_log_yield_wanted(), "log connection asked to yield while the TLS sockets were taken, not after");

    long off2 = off + 1000;
    printf("T8b resumed download from %ld, not on a 4096-byte boundary, reads of 4096 bytes\n", off2);
    reset();
    chunk = 4096;
    int h8b = sprintf(f2, "HTTP/1.1 206 Partial\r\nContent-Length: %ld\r\nContent-Range: bytes %ld-%ld/%ld\r\n\r\n", z - off2, off2, z - 1, z);
    memcpy(f2 + h8b, file + h7 + off2, z - off2);
    serve(f2, h8b + z - off2);
    cloud_content(uid, NULL, off2, 0xC0FFE1);
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    n_writes = 0;
    content_download(&j);
    got = fopen(q, "rb");
    CHECK(got && fread(gb, 1, z, got) == (size_t)z && !memcmp(gb, file + h7, z), "file content equal");
    fclose(got);
    CHECK(cloud_dl.done && n_writes == (int)(1 + (z - (off + 4096) + 4095) / 4096), "first piece up to the boundary (%d writes)", n_writes);
    chunk = 1400;

    printf("T8c connection closed after 10000 bytes: what arrived is kept\n");
    reset();
    static const uint8_t uidC[8] = {0x77, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0};
    serve(file, h7 + 10000);
    close_at_end = true;
    cloud_content(uidC, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    content_download(&j);
    char qc[600];
    full("CONTENT/77223344/500304E0", qc);
    got = fopen(qc, "rb");
    CHECK(got && fread(gb, 1, 10000, got) == 10000 && !memcmp(gb, file + h7, 10000), "first 10000 bytes on the card");
    if (got)
    {
        fclose(got);
    }
    CHECK(cloud_dl.failed && cloud_dl.pos == 10000, "failed at %u", cloud_dl.pos);

    printf("T9 content 404\n");
    reset();
    serve("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0);
    static const uint8_t uid2[8] = {0x99, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0};
    cloud_content(uid2, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    content_download(&j);
    CHECK(cloud_dl.failed && cloud_dl.error == CE_REFUSED && cloud_dl.status == 404, "content refused");

    static const uint8_t uidA[8] = {0x11, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0}, uidB[8] = {0x55, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0};
    printf("T10 download A overtaken by B during the body (B1)\n");
    reset();
    serve(file, h7 + z);
    recv_calls = 0;
    {
        void hook(int n)
        {
            if (n == 100)
            {
                cloud_content(uidB, NULL, 0, 0);
            }
        }
        cloud_content(uidA, NULL, 0, 0);
        xQueueReceive(queue, &j, 0);
        dl_cancel = false;
        recv_hook = hook;
        content_download(&j);
        recv_hook = NULL;
    }
    CHECK(!cloud_dl.failed && cloud_dl.active && cloud_dl.readable == 0 && cloud_dl.pos == 0 && !memcmp(cloud_dl.uid, uidB, 8), "B's state untouched by A");
    CHECK(xQueueReceive(queue, &j, 0) && j.serial == cloud_dl.serial, "B queued");
    cloud_content_cancel();
    dl_cancel = false;
    content_download(&j);
    CHECK(!cloud_dl.active && n_sent == 1, "cancelled queued job: skipped, not active (B1)");

    printf("T11 content request A, server silent before the header, B placed (B3)\n");
    reset();
    serve("", 0);
    answer_len[0] = 0;
    recv_calls = 0;
    {
        void hook(int n)
        {
            if (n == 3)
            {
                cloud_content(uidB, NULL, 0, 0);
            }
        }
        uint32_t t0 = sim_ms;
        cloud_content(uidA, NULL, 0, 0);
        xQueueReceive(queue, &j, 0);
        dl_cancel = false;
        recv_hook = hook;
        content_download(&j);
        recv_hook = NULL;
        CHECK(sim_ms - t0 < 10000 && cloud_dl.active && !cloud_dl.failed, "A abandoned at once (%u ms), B untouched", sim_ms - t0);
        xQueueReceive(queue, &j, 0);
    }

    printf("T12 retry from the download position keeps the readable part (B2)\n");
    reset();
    cloud_content(uidA, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    cloud_dl.pos = 70000;
    cloud_dl.readable = 69632;
    cloud_dl.failed = true;
    cloud_dl.active = false;
    cloud_content(uidA, NULL, cloud_dl.pos, 0xC0FFEE);
    xQueueReceive(queue, &j, 0);
    CHECK(cloud_dl.pos == 70000 && cloud_dl.readable == (70000u & ~511u) && j.offset == 70000, "pos %u readable %u", cloud_dl.pos, cloud_dl.readable);
    session = 0;
    dl_cancel = false;
    content_download(&j);
    session = 1;
    CHECK(cloud_dl.failed && cloud_dl.error == CE_CANCELLED && cloud_dl.pos == 70000, "no session: cancelled, not name resolution (B5); pos kept (%s)", cloud_error_name(cloud_dl.error));
    cloud_content(uidA, NULL, cloud_dl.pos, 0xC0FFEE);
    xQueueReceive(queue, &j, 0);
    CHECK(j.offset == 70000, "next retry still ranged");
    cloud_content(uidB, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    CHECK(cloud_dl.pos == 0 && cloud_dl.readable == 0, "a new figure starts at 0");

    printf("T13 full card: file extended only partly (B9)\n");
    reset();
    serve(file, h7 + z);
    lseek_limit = 32768;
    cloud_content(uidB, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    dl_cancel = false;
    content_download(&j);
    lseek_limit = 0;
    CHECK(cloud_dl.failed && f_stat("CONTENT/55223344/500304E0", &fi) != FR_OK && !cloud_download_file_open(), "download abandoned, short file trashed, file closed");

    printf("T14 packed field 1 not a multiple of 8 (B7)\n");
    {
        static const uint8_t b2[] = {0x0A, 0x04, 1, 2, 3, 4, 0x18, 0x01, 0x20, 0x00}, b3[] = {0x0A, 0x04, 1, 2, 3, 4}, b4[] = {0x0A, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0x18, 0x01};
        static fresh_t r;
        CHECK(!fresh_decode(b2, sizeof b2, &r), "4-byte list followed by field 3 rejected");
        CHECK(!fresh_decode(b3, sizeof b3, &r), "4-byte list at the end rejected");
        CHECK(fresh_decode(b4, sizeof b4, &r) && r.n_marks == 1 && r.value[3] == 1, "8-byte list accepted");
    }

    printf("T15 user sync while a download runs fails at once (B8)\n");
    cloud_content(uidA, NULL, 0, 0);
    xQueueReceive(queue, &j, 0);
    sync_res = -99;
    sync_pending = false;
    cloud_sync_request(true);
    CHECK(sync_res == CE_CANCELLED && !sync_pending, "result cancelled at once, nothing pending");
    printf("T16 claim not sent while a transfer runs (B15)\n");
    cloud_content_cancel();
    unsigned q0 = uxQueueMessagesWaiting(queue);
    sync_running = true;
    cloud_claim(uidA, NULL);
    sync_running = false;
    job_running = true;
    cloud_claim(uidA, NULL);
    job_running = false;
    CHECK(uxQueueMessagesWaiting(queue) == q0, "no claim queued");
    cloud_claim(uidA, NULL);
    CHECK(uxQueueMessagesWaiting(queue) == q0 + 1, "claim queued when idle");
    xQueueReceive(queue, &j, 0);

    printf("T16b sync marks the box busy and ends idle\n");
    reset();
    session = 1;
    state = 6;
    sync_pending = true;
    box_busy = -1;
    serve("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0);
    for (int i = 0; i < 3; i++)
    {
        serve("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0);
    }
    {
        void hook(int n)
        {
            (void)n;
            if (!cloud_sync_running() || box_busy != 1)
            {
                box_busy = 99;
            }
        }
        recv_hook = hook;
        maybe_sync();
        recv_hook = NULL;
    }
    CHECK(n_sent >= 1 && box_busy == 0 && !cloud_sync_running(), "busy and running during the requests, idle after (%d)", box_busy);
    printf("T17 sync with net state 6 but no session not started\n");
    reset();
    session = 0;
    state = 6;
    sync_pending = true;
    maybe_sync();
    CHECK(n_sent == 0 && sync_pending, "nothing sent, still pending");
    session = 1;
    sync_pending = false;

    printf("T18 network processor refuses the date: time base unchanged (B12)\n");
    reset();
    serve("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n1900000000", 0);
    devset_fail = 1;
    ok = time_request(1);
    devset_fail = 0;
    CHECK(!ok && clock_now() < 1800000000, "clock %u", clock_now());

    printf("T19 HW/ word (B14)\n");
    reset();
    serve("HTTP/1.1 304 x\r\n\r\n", 0);
    serve("HTTP/1.1 304 x\r\n\r\n", 0);
    serve("HTTP/1.1 304 x\r\n\r\n", 0);
    {
        conn_t c;
        ho.bootloader_word = 0x00110203;
        exchange(&c, 1, 0, NULL, "GET", "/a", NULL, NULL, 0);
        close_conn(&c);
        bb_sram[104] = 4;
        bb_sram[106] = 0x11;
        exchange(&c, 1, 0, NULL, "GET", "/b", NULL, NULL, 0);
        close_conn(&c);
        ho.bootloader_word = 0;
        memset(bb_sram, 0, sizeof bb_sram);
        exchange(&c, 1, 0, NULL, "GET", "/c", NULL, NULL, 0);
        close_conn(&c);
        CHECK(strstr(sent[0], " HW/1114627\r\n") && strstr(sent[1], " HW/1114116\r\n") && strstr(sent[2], " HW/0\r\n"), "handover word, build-block word first, 0 without both");
    }

    printf("T20 log connection: one name lookup\n");
    cfg.is_ip = false;
    dns_fail = 1;
    dns_calls = 0;
    CHECK(cloud_log_connect(1) < 0 && dns_calls == 1, "%d lookup(s)", dns_calls);
    dns_calls = 0;
    reset();
    {
        conn_t c;
        exchange(&c, 1, 0, NULL, "GET", "/x", NULL, NULL, 0);
        CHECK(dns_calls == 3, "API request: 3 lookups");
    }
    cfg.is_ip = true;
    dns_fail = 0;

    printf("T21 70 valid content files and 1 invalid in one directory (B4)\n");
    reset();
    {
        char q[1300], d[600];
        full("CONTENT", d);
        system((snprintf(q, sizeof q, "rm -rf '%s'/*", d), q));
        full("CONTENT/AABBCC00", d);
        mkdir(d, 0755);
        for (int i = 0; i < 71; i++)
        {
            char nm[700];
            snprintf(nm, sizeof nm, "%s/%08X", d, 0x10000000 + i);
            FILE *w = fopen(nm, "wb");
            if (i == 30)
            {
                fwrite("broken", 1, 6, w);
            }
            else
            {
                fwrite(file + h7, 1, z, w);
            }
            fclose(w);
        }
        for (int i = 0; i < 4; i++)
        {
            serve("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0);
        }
        e = fresh_session(1);
        int figs = 0;
        for (int i = 0; i < n_sent; i++)
        {
            const char *bp = strstr(sent[i], "\r\n\r\n") + 4;
            size_t bl = sent_len[i] - (bp - sent[i]);
            for (size_t k = 0; k + 16 <= bl; k += 16)
            {
                if (bp[k + 3] || bp[k + 4] || bp[k + 5])
                {
                    figs++;
                }
            }
        }
        CHECK(e == CE_OK && figs == 70, "all 70 valid figures reported (%d)", figs);
        system((snprintf(q, sizeof q, "rm -rf '%s'", d), q));
    }

    ext_tests(file + h7, z);

    printf("T25 first cloud exchange result\n");
    {
        reset();
        update_checked = true;
        serve("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n1791776107", 0);
        online_round(1);
        CHECK(first_result == CE_OK, "success: CE_OK");
        reset();
        cfg.is_ip = false;
        dns_fail = 1;
        online_round(1);
        dns_fail = 0;
        cfg.is_ip = true;
        CHECK(first_result == CE_NAME, "name lookup failed: CE_NAME");
        reset();
        online_round(1);
        CHECK(first_result == CE_CONNECT, "connect refused: CE_CONNECT");
        reset();
        socket_err = SL_ENOBUFS;
        online_round(1);
        socket_err = 0;
        CHECK(first_result == CE_CONNECT && cloud_restart_wanted(), "out of buffers: CE_CONNECT, restart wanted");
        reset();
        serve("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n0", 0);
        online_round(1);
        CHECK(first_result == CE_PROTOCOL, "time 0 not applied: other failure (%d)", first_result);
        reset();
        serve("HTTP/1.1 404 x\r\nContent-Length: 0\r\n\r\n", 0);
        online_round(1);
        CHECK(first_result == CE_PROTOCOL, "404: other failure");
        cfg.valid = false;
        online_round(1);
        cfg.valid = true;
        CHECK(first_result == CE_NAME, "no configuration: CE_NAME");
    }

    printf("T26 remote update check (command 73)\n");
    {
        job_t jj;
        while (xQueueReceive(queue, &jj, 0))
            ;
        session = 1;
        state = 4;
        CHECK(cloud_remote_update_check() == 1, "state 4 (time not applied): 1");
        state = 6;
        CHECK(cloud_remote_update_check() == 0 && xQueueReceive(queue, &jj, 0) && jj.kind == JOB_UPDATE && jj.session == 1, "idle: scheduled");
        job_running = true;
        CHECK(cloud_remote_update_check() == 1, "request in progress: 1");
        job_running = false;
        sync_running = true;
        CHECK(cloud_remote_update_check() == 1, "sync running: 1");
        sync_running = false;
        cloud_claim(uidA, NULL);
        CHECK(cloud_remote_update_check() == 1, "job waiting: 1");
        while (xQueueReceive(queue, &jj, 0))
            ;
        session = 0;
        CHECK(cloud_remote_update_check() == 1, "no network session: 1");
        session = 1;
        CHECK(uxQueueMessagesWaiting(queue) == 0, "nothing queued when refused");
    }

    printf("T22 certificate digests\n");
    {
        static const uint8_t abc[] = "abc";
        static uint8_t big[5000];
        for (int i = 0; i < 5000; i++)
        {
            big[i] = (uint8_t)i;
        }
        flash[0].name = "/cert/client.der";
        flash[0].data = abc;
        flash[0].len = 3;
        flash[1].name = "/cert/private.der";
        flash[1].data = big;
        flash[1].len = 0;
        n_flash = 2;
        n_fn = 0;
        fn_any = 1;
        cert_digests();
        CHECK(n_fn == 3 && fn_func[0] == 8656 && fn_args[0].n == 36 && ext_le32(fn_args[0].data) == 8649 && ext_le32(fn_args[0].data + 4) == 0xba7816bfu && ext_le32(fn_args[0].data + 8) == 0x8f01cfeau && ext_le32(fn_args[0].data + 12) == 0x414140deu && ext_le32(fn_args[0].data + 32) == 0xf20015adu, "client: SHA-256 of abc, argument 4 from bytes 9 9 11 12");
        CHECK(fn_func[1] == 8375 && fn_var[1] == 1 && fn_args[1].n == 0, "empty private key: 8375");
        CHECK(fn_func[2] == 8659 && ext_le32(fn_args[2].data) == 8651 && (int32_t)ext_le32(fn_args[2].data + 4) == -11, "CA (%s) missing: 8659", cfg.ca);
        flash[1].len = 5000;
        n_fn = 0;
        cert_digests();
        CHECK(n_fn == 3 && fn_func[1] == 8656 && ext_le32(fn_args[1].data) == 8650, "5000-byte file read in pieces");
        n_flash = 0;
        fn_any = 0;
    }

    printf("T23 log-connection outcome post\n");
    {
        cfg.is_ip = false;
        dns_fail_once = 1;
        session = 1;
        reset();
        n_fn = 0;
        fn_any = 1;
        cloud_log_connect(1);
        serve("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0);
        log_post();
        CHECK(n_sent == 1 && strstr(sent[0], "POST /v1/log HTTP/1.1") && strstr(sent[0], "Content-Length: 8\r\n") && !memcmp(strstr(sent[0], "\r\n\r\n") + 4, "998 (-1)", 8), "998 (-1) posted");
        log_post();
        dns_fail_once = 1;
        cloud_log_connect(1);
        log_post();
        CHECK(n_sent == 1, "same outcome again: dropped");
        cfg.is_ip = true;
        serve("x", 1);
        int16_t sk = cloud_log_connect(1);
        log_post();
        CHECK(sk >= 0 && n_sent == 2, "992 while the log connection is open: dropped");
        cloud_log_closed();
        cloud_log_connect(1); /* no answer left: connect refused */
        serve("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0);
        log_post();
        CHECK(n_sent == 3 && !memcmp(strstr(sent[2], "\r\n\r\n") + 4, "976 (-111)", 10), "976 (-111) posted");
        CHECK(n_fn == 1 && fn_func[0] == 116 && fn_var[0] == 2 && fn_args[0].n == 4 + 7 + 4 && ext_le32(fn_args[0].data + 11) == 404, "status 404: record 116 variant b (%d records)", n_fn);
        cloud_log_closed();
        cfg.is_ip = false;
        dns_fail_once = 1;
        cloud_log_connect(1);
        n_fn = 0;
        log_post();
        cfg.is_ip = true;
        CHECK(n_fn == 1 && fn_func[0] == 7006 && fn_var[0] == 6, "no answer: record 7006 variant f");
        fn_any = 0;
    }

    printf("T24 bad-block statistics\n");
    {
        char q[600];
        full("BADBLOCK", q);
        mkdir(q, 0755);
        full("BADBLOCK/123", q);
        mkdir(q, 0755);
        full("BADBLOCK/123/SUB", q);
        mkdir(q, 0755);
        const char *fs[4] = {"BADBLOCK/123/A.BIN", "BADBLOCK/123/B.BIN", "BADBLOCK/TOP.BIN", "BADBLOCK/123/SUB/C.BIN"};
        long sz[4] = {100, 2000, 7, 9};
        time_t tm[4] = {1700000000, 1790000002, 1, 1};
        for (int i = 0; i < 4; i++)
        {
            full(fs[i], q);
            FILE *w = fopen(q, "wb");
            for (long k = 0; k < sz[i]; k++)
            {
                fputc(0, w);
            }
            fclose(w);
            struct utimbuf u = {tm[i], tm[i]};
            utime(q, &u);
        }
        n_fn = 0;
        fn_any = 1;
        badblock_stats();
        CHECK(n_fn == 1 && fn_func[0] == 8608 && ext_le32(fn_args[0].data) == 2 && ext_le32(fn_args[0].data + 4) == 2100 && ext_le32(fn_args[0].data + 8) == 1700000000u && ext_le32(fn_args[0].data + 12) == 1790000002u, "2 files, 2100 bytes, oldest and newest");
        full("BADBLOCK/123/A.BIN", q);
        struct stat st;
        CHECK(!stat(q, &st) && st.st_size == 100, "card unchanged");
        system((snprintf(q, sizeof q, "rm -rf '%s/BADBLOCK'", root), q));
        n_fn = 0;
        badblock_stats();
        CHECK(n_fn == 0, "no BADBLOCK: no record");
        fn_any = 0;
    }
    printf("T27 server configuration check for the setup page\n");
    {
        static const struct
        {
            const char *text, *why;
        } t[] = {
            {"host=192.0.2.10\nport=8443\n", NULL}, {"# c\nfoo=1\n HOST = tc.example. \r\n", NULL}, {"", NULL}, {"foo=1\n", NULL}, {"host=prod.de.tbs.toys\n", "vendor"}, {"host=RTNL.BXCL.DE\n", "vendor"}, {"host=tbs.toys.\n", "vendor"}, {"host=notbs.toys\n", NULL}, {"host=a..b\n", "host"}, {"host=-a\n", "host"}, {"host=a_b\n", "host"}, {"host=\n", "host"}, {"host=x\nport=0\n", "port"}, {"host=x\nport=65536\n", "port"}, {"host=x\nport=\n", "port"}, {"host=x\nport=65535\n", NULL}, {"host=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", NULL}, {"host=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", "host"}, {"host=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", "host"}, {"host=bad name\nhost=10.0.0.5\n", NULL}};
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        {
            char b[200];
            strcpy(b, t[i].text);
            const char *w = cloud_config_check(b);
            CHECK(t[i].why ? w && !strcmp(w, t[i].why) : !w, "\"%.30s...\" -> %s", t[i].text, w ? w : "ok");
        }
    }

    printf("T28 certificate from the setup page (check and store)\n");
    {
        strcpy(cfg.ca, "/cert/c2.der");
        n_flash = 0;
        fs_writes = 0;
        uint8_t d[300];
        memset(d, 7, sizeof d);
        d[0] = 0x30;
        d[1] = 0x82;
        d[2] = 0x01;
        d[3] = 0x28; /* 4 + 296 = 300 */
        memcpy(work, d, 300);
        CHECK(cloud_cert_put(2, 300) == 0 && n_flash == 1 && !strcmp(flash[0].name, "/cert/c2.der") && flash[0].len == 300 && !memcmp(wbuf[0], d, 300), "ca.der written to the CA file of the key ca");
        memcpy(work, d, 300);
        CHECK(cloud_cert_put(2, 300) == 0 && fs_writes == 1, "equal: not written again");
        memcpy(work, d, 300);
        work[10] = 9;
        CHECK(cloud_cert_put(2, 300) == 0 && fs_writes == 2 && wbuf[0][10] == 9, "different: replaced");
        memcpy(work, d, 300);
        CHECK(cloud_cert_put(0, 299) == -1 && cloud_cert_put(0, 301) == -1 && n_flash == 1, "length not the size: refused, nothing written");
        work[0] = 0x31;
        CHECK(cloud_cert_put(1, 300) == -1, "tag not 0x30: refused");
        uint8_t s[2] = {0x30, 0x00};
        memcpy(work, s, 2);
        CHECK(cloud_cert_put(1, 2) == 0 && !strcmp(flash[1].name, "/cert/client.der") && flash[1].len == 2, "2-byte element: written to client.der");
        memcpy(work, "\x30\x81\x02\x00\x00", 5);
        CHECK(cloud_cert_put(0, 5) == 0 && !strcmp(flash[2].name, "/cert/private.der"), "0x81 form: written to private.der");
        CHECK(cloud_cert_put(0, 1) == -1 && cloud_cert_put(0, 4097) == -1 && cloud_cert_put(3, 5) == -1, "1 byte, 4097 bytes, index 3: refused");
        memcpy(work, d, 300);
        work[20] = 1;
        fs_write_bad = 1;
        CHECK(cloud_cert_put(2, 300) == 1, "read-back differs: failed");
        fs_write_bad = 0;
        fs_write_fail = 1;
        work[20] = 2;
        CHECK(cloud_cert_put(2, 300) == 1, "write fails: failed");
        fs_write_fail = 0;
        char q[600];
        full("cfw", q);
        mkdir(q, 0755);
        full("cfw/cert", q);
        mkdir(q, 0755);
        full("cfw/cert/ca.der", q);
        FILE *w = fopen(q, "wb");
        fwrite(d, 1, 300, w);
        fclose(w);
        full("cfw/cert/client.der", q);
        w = fopen(q, "wb");
        fwrite(d, 1, 299, w);
        fclose(w);
        cert_input();
        struct stat st;
        full("cfw/cert/ca.der", q);
        bool gone = stat(q, &st) != 0;
        full("cfw/cert/client.der", q);
        CHECK(gone && !stat(q, &st) && !memcmp(wbuf[0], d, 300), "start: card ca.der written and deleted, invalid client.der left");
        system((snprintf(q, sizeof q, "rm -rf '%s/CFW'", root), q));
        for (int i = 0; i < n_flash; i++)
        {
            free((void *)flash[i].name);
        }
        n_flash = 0;
    }

    printf("T29 start without /cfw/config.txt: no cloud, default CA\n");
    {
        char q[600];
        full("cfw/config.txt", q);
        unlink(q);
        config_read();
        CHECK(!cfg.valid && !strcmp(cfg.ca, "/cert/ca.der"), "no server, CA %s", cfg.ca);
        full("cfw", q);
        mkdir(q, 0755);
        full("cfw/config.txt", q);
        FILE *w = fopen(q, "wb");
        fputs("host=tc.example\nport=8443\nca=c2.der\n", w);
        fclose(w);
        config_read();
        CHECK(cfg.valid && !strcmp(cfg.host, "tc.example") && cfg.port == 8443 && !strcmp(cfg.ca, "/cert/c2.der"), "server read");
        w = fopen(q, "wb");
        for (int i = 0; i < 1025; i++)
        {
            fputc('#', w);
        }
        fclose(w);
        config_read();
        CHECK(!cfg.valid && !strcmp(cfg.ca, "/cert/ca.der"), "1025 bytes: no server");
        unlink(q);
        full("cfw", q);
        rmdir(q);
    }
    printf("T30 identity of the box's own original firmware\n");
    {
        static uint8_t bootinfo[8] = {1, 0, 0, 0, 0xBA, 0xDC, 0xCD, 0xAB};
        static uint8_t slot[400];
        memset(slot, 0, sizeof slot);
        uint8_t *bb = slot + sizeof slot - 176;
        bb[0] = 0x05, bb[1] = 0x00, bb[2] = 0xAC, bb[3] = 0xBE;
        bb[108] = 0x05, bb[109] = 0x00, bb[110] = 0xAC, bb[111] = 0xBE;
        bb[8] = 0xA9, bb[9] = 0x33, bb[10] = 0x94, bb[11] = 0x60; /* 1620325289 */
        n_flash = 2;
        flash[0].name = "/sys/mcubootinfo.bin";
        flash[0].data = bootinfo;
        flash[0].len = 8;
        flash[1].name = "/sys/mcuimg2.bin";
        flash[1].data = slot;
        flash[1].len = sizeof slot;
        cloud_build_read();
        memset(&cfg, 0, sizeof cfg);
        cfg.valid = cfg.is_ip = true;
        cfg.ip = 0xC0A8057A;
        strcpy(cfg.host, "192.0.2.10");
        cfg.port = 443;
        strcpy(cfg.ca, "/cert/c2.der");
        reset();
        serve("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n1791776107", 0);
        time_request(1);
        CHECK(strstr(sent[0], "User-Agent: TB/1620325289 SP/") && current_version(3) == 1620325289,
              "User-Agent and item 3 carry the build of the slot the boot info names");
        bootinfo[0] = 2; /* /sys/mcuimg3.bin is missing */
        cloud_build = CLOUD_BUILD;
        cloud_build_read();
        CHECK(cloud_build == CLOUD_BUILD, "no build block in that slot: 3.4.0");
        n_flash = 0;
    }
    printf("\n%d failure(s)\n", failures);
    return failures != 0;
}
#endif
