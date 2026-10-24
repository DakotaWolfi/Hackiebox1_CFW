/*
 * Host test of the setup page server (page.c): scripted
 * connections through socket stubs, the card as a host directory, net and
 * cloud stubs (the Wi-Fi fields go through the real setup_post). Build and
 * run: tests/host/page/run.sh
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define CFW_BUILD_TIME 1792000000
#define CFW_VERSION "9.9.9"
#include "page.c"

static int failures;
#define CHECK(c, ...)                        \
    do                                       \
    {                                        \
        if (!(c))                            \
        {                                    \
            printf("  FAIL %d: ", __LINE__); \
            failures++;                      \
        }                                    \
        else                                 \
        {                                    \
            printf("  ok: ");                \
        }                                    \
        printf(__VA_ARGS__);                 \
        printf("\n");                        \
    } while (0)

/* --- time, monitor, debug --- */
static uint32_t now;
static int marks;
uint32_t tick_ms(void) { return now; }
void vTaskDelay(TickType_t t) { now += t; }
void monitor_mark(activity_t a)
{
    (void)a;
    marks++;
}
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}

/* --- card: FatFs over a host directory, 8.3 names in upper case --- */
static const char *root;
static void full(const char *p, char *o)
{
    snprintf(o, 600, "%s/%s", root, p[0] == '/' ? p + 1 : p);
    for (char *c = o + strlen(root); *c; c++)
    {
        *c = (char)toupper((unsigned char)*c);
    }
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
    f->size = (FSIZE_t)ftell(f->fp);
    rewind(f->fp);
    return FR_OK;
}
FRESULT f_read(FIL *f, void *b, UINT n, UINT *g)
{
    *g = (UINT)fread(b, 1, n, f->fp);
    return FR_OK;
}
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w)
{
    *w = (UINT)fwrite(b, 1, n, f->fp);
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
    memset(fi, 0, sizeof *fi);
    return stat(q, &st) ? FR_NO_FILE : FR_OK;
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
    struct stat st;
    if (!stat(r, &st))
    {
        return FR_EXIST; /* FatFs does not replace */
    }
    return rename(q, r) ? FR_DENIED : FR_OK;
}
FRESULT f_mkdir(const char *p)
{
    char q[600];
    full(p, q);
    return mkdir(q, 0755) ? FR_EXIST : FR_OK;
}

static void put(const char *p, const void *d, size_t n)
{
    char q[600];
    full(p, q);
    FILE *w = fopen(q, "wb");
    fwrite(d, 1, n, w);
    fclose(w);
}
static size_t get(const char *p, char *d, size_t max)
{
    char q[600];
    full(p, q);
    FILE *r = fopen(q, "rb");
    if (!r)
    {
        return (size_t)-1;
    }
    size_t n = fread(d, 1, max, r);
    fclose(r);
    d[n < max ? n : max - 1] = 0;
    return n;
}

/* --- net --- */
static bool allowed = true, active;
static int ends, took;
bool net_page_begin(void)
{
    if (!allowed || active)
    {
        return false;
    }
    return active = true;
}
bool net_page_on(void) { return allowed; }
void net_page_end(bool t)
{
    active = false;
    ends++;
    took += t;
}
static int rescans, rescan_answered;
static int conn_answered(void);
void net_setup_rescan(void)
{
    rescans++;
    rescan_answered += conn_answered();
}
static setup_creds_t creds;
static int submits;
void net_setup_field(const uint8_t *n, unsigned nl, const uint8_t *v, unsigned vl)
{
    if (setup_post(&creds, n, nl, v, vl) == POST_SUBMIT && setup_form(&creds))
    {
        submits++;
    }
}
static scan_entry_t scan[3];
static unsigned scan_n;
unsigned net_scan_list(const scan_entry_t **l)
{
    *l = scan;
    return scan_n;
}

static uint8_t language = 1;
uint8_t settings_language(void)
{
    return language;
}
void settings_set_language(uint8_t l)
{
    if (l <= 3)
    {
        language = l;
    }
}

/* --- cloud (the activity's buffers are these) --- */
static uint8_t work[4096];
static char buf[1400];
static uint8_t cert_got[4096];
static unsigned cert_len, cert_calls;
static int cert_i = -1, cert_ret;
int cloud_cert_put(unsigned i, unsigned size)
{
    cert_i = (int)i;
    cert_len = size;
    cert_calls++;
    memcpy(cert_got, work, size);
    return cert_ret;
}
static int resets_wifi, resets_content, resets_server;
long net_profiles_delete(void)
{
    resets_wifi++;
    return 0;
}
void sd_delete_content(void) { resets_content++; }
const char *cloud_server_forget(void)
{
    resets_server++;
    return "ok";
}
const char *cloud_config_check(char *t) { return strstr(t, "tbs.toys") ? "vendor" : NULL; }

/* --- sockets: connections 10, 11, ... in the order scripted --- */
typedef struct
{
    const char *req;
    size_t len, pos;
    char out[12000];
    size_t out_len;
    int closed;
} conn_t;
static conn_t conns[24];
static int n_conns, next_conn, accept_eagain = 2, http_stops, chunk = 50;
int16_t sl_Socket(int16_t d, int16_t t, int16_t p)
{
    (void)d;
    (void)t;
    (void)p;
    return 3;
}
int16_t sl_Bind(int16_t s, const SlSockAddr_t *a, int16_t l)
{
    (void)s;
    (void)a;
    (void)l;
    return 0;
}
int16_t sl_Listen(int16_t s, int16_t n)
{
    (void)s;
    (void)n;
    return 0;
}
int16_t sl_SetSockOpt(int16_t s, int16_t l, int16_t o, const void *v, SlSocklen_t n)
{
    (void)s;
    (void)l;
    (void)o;
    (void)v;
    (void)n;
    return 0;
}
int16_t sl_NetAppStop(uint32_t m)
{
    (void)m;
    http_stops++;
    return 0;
}
int16_t sl_Accept(int16_t s, SlSockAddr_t *a, SlSocklen_t *l)
{
    (void)s;
    (void)a;
    (void)l;
    if (accept_eagain)
    {
        accept_eagain--;
        return SL_EAGAIN;
    }
    if (next_conn >= n_conns)
    {
        allowed = false; /* setup mode ends (net_page_on) */
        return SL_EAGAIN;
    }
    return (int16_t)(10 + next_conn++);
}
int16_t sl_Recv(int16_t s, void *b, int16_t n, int16_t f)
{
    (void)f;
    conn_t *c = &conns[s - 10];
    if (c->pos >= c->len)
    {
        now += 1000; /* receive timeout */
        return SL_EAGAIN;
    }
    size_t k = c->len - c->pos;
    if (k > (size_t)n)
    {
        k = (size_t)n;
    }
    if (k > (size_t)chunk)
    {
        k = (size_t)chunk;
    }
    memcpy(b, c->req + c->pos, k);
    c->pos += k;
    return (int16_t)k;
}
int16_t sl_Send(int16_t s, const void *b, int16_t n, int16_t f)
{
    (void)f;
    conn_t *c = &conns[s - 10];
    if (c->out_len + (size_t)n < sizeof c->out)
    {
        memcpy(c->out + c->out_len, b, (size_t)n);
    }
    c->out_len += (size_t)n;
    return n;
}
int16_t sl_Close(int16_t s)
{
    if (s >= 10)
    {
        conns[s - 10].closed++;
    }
    return 0;
}

static int conn(const char *req, size_t len)
{
    conns[n_conns].req = req;
    conns[n_conns].len = len ? len : strlen(req);
    return n_conns++;
}
/* the connection accepted last has its answer */
static int conn_answered(void) { return next_conn > 0 && conns[next_conn - 1].out_len > 0; }
/* The body of connection i's answer (after the head), "" when none. */
static const char *body(int i)
{
    conns[i].out[conns[i].out_len < sizeof conns[i].out ? conns[i].out_len : 0] = 0;
    char *e = strstr(conns[i].out, "\r\n\r\n");
    return e ? e + 4 : "";
}
static bool is_type(int i, const char *t)
{
    char h[64];
    snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: text/%s\r\n\r\n", t);
    return !strncmp(conns[i].out, h, strlen(h));
}

int main(int argc, char **argv)
{
    root = argc > 1 ? argv[1] : ".";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("P1 no /cfw/setup.htm on the card: the vendor page stays\n");
    page_run(work, sizeof work, buf, sizeof buf);
    CHECK(ends == 1 && !took && !http_stops, "not started, HTTP server not stopped");

    printf("P2 requests\n");
    char q[600];
    full("cfw", q);
    mkdir(q, 0755);
    static char page[5000];
    for (unsigned i = 0; i < sizeof page; i++)
    {
        page[i] = (char)('a' + i % 26);
    }
    put("/cfw/setup.htm", page, sizeof page);
    const char *cfg0 = "# box\nhost=old.example\nport=443\nca=c2.der\n";
    put("/cfw/config.txt", cfg0, strlen(cfg0));
    scan[0] = (scan_entry_t){"hi\\\"", 4, 3, -40};
    scan[1] = (scan_entry_t){"", 0, 0, -60};
    scan_n = 2;

    int c_page = conn("GET / HTTP/1.1\r\nHost: toniebox.start\r\nAccept: */*\r\n\r\n", 0);
    int c_scan = conn("GET /scan.txt HTTP/1.1\r\n\r\n", 0);
    int c_cfg = conn("GET /config.txt?t=1 HTTP/1.1\r\n\r\n", 0);
    int c_lang = conn("GET /lang.txt HTTP/1.1\r\n\r\n", 0);
    int c_version = conn("GET /version.txt HTTP/1.1\r\n\r\n", 0);
    int c_lang_set = conn("POST /lang.txt HTTP/1.1\r\nContent-Length: 1\r\n\r\n2", 0);
    int c_lang_bad = conn("POST /lang.txt HTTP/1.1\r\nContent-Length: 1\r\n\r\n7", 0);
    static const char new_cfg[] = "# box\nca=c2.der\nhost=192.0.2.10\nport=8443\n";
    static char r_cfg[300], r_vendor[300], r_long[1400], r_cert[700], r_cert_big[200], r_wifi[400];
    snprintf(r_cfg, sizeof r_cfg, "POST /config.txt HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", strlen(new_cfg), new_cfg);
    int c_post = conn(r_cfg, 0);
    snprintf(r_vendor, sizeof r_vendor, "POST /config.txt HTTP/1.1\r\nContent-Length: 19\r\n\r\nhost=prod.tbs.toys\n");
    int c_vendor = conn(r_vendor, 0);
    int n = snprintf(r_long, sizeof r_long, "POST /config.txt HTTP/1.1\r\nContent-Length: 1025\r\n\r\n");
    memset(r_long + n, '#', 1025);
    int c_long = conn(r_long, (size_t)n + 1025);
    uint8_t der[300];
    for (int i = 0; i < 300; i++)
    {
        der[i] = (uint8_t)i;
    }
    n = snprintf(r_cert, sizeof r_cert, "POST /cert/2 HTTP/1.1\r\nContent-Type: application/octet-stream\r\nContent-Length: 300\r\n\r\n");
    memcpy(r_cert + n, der, 300);
    int c_cert = conn(r_cert, (size_t)n + 300);
    int c_cert3 = conn("POST /cert/3 HTTP/1.1\r\nContent-Length: 2\r\n\r\n\x30\x00", 46);
    snprintf(r_cert_big, sizeof r_cert_big, "POST /cert/0 HTTP/1.1\r\nContent-Length: 4097\r\n\r\n");
    int c_big = conn(r_cert_big, 0);
    snprintf(r_wifi, sizeof r_wifi, "POST /wifi HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", strlen("__SL_P_USL=&__SL_P_USN=&__SL_P_USR=4E31&__SL_P_USS=&__SL_P_USP=7365637265742121&__SL_P_USQ=&__SL_P_USM=3&__SL_P_UST=3&__SL_P_USO="), "__SL_P_USL=&__SL_P_USN=&__SL_P_USR=4E31&__SL_P_USS=&__SL_P_USP=7365637265742121&__SL_P_USQ=&__SL_P_USM=3&__SL_P_UST=3&__SL_P_USO=");
    int c_wifi = conn(r_wifi, 0);
    int c_bad = conn("HELLO\r\n\r\n", 0);
    static char r_huge[1500];
    memset(r_huge, 'x', sizeof r_huge - 1);
    int c_huge = conn(r_huge, 0);
    int c_silent = conn("GET / HTTP/1.1\r\n", 0);
    int c_page2 = conn("GET /setup.htm HTTP/1.1\r\n\r\n", 0);
    int c_rwifi = conn("POST /reset/wifi HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 0);
    int c_rcontent = conn("POST /reset/content HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 0);
    int c_rserver = conn("POST /reset/server HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 0);
    int c_rescan = conn("POST /rescan HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 0);

    page_run(work, sizeof work, buf, sizeof buf);
    CHECK(ends == 2 && took == 1 && http_stops == 1, "served until setup mode ended; HTTP server stopped once");
    int all_closed = 1;
    for (int i = 0; i < n_conns; i++)
    {
        all_closed &= conns[i].closed == 1;
    }
    CHECK(all_closed, "every connection closed once");
    CHECK(is_type(c_page, "html") && strlen(body(c_page)) == sizeof page && !memcmp(body(c_page), page, sizeof page),
          "GET /: the page from the card, 5000 bytes");
    CHECK(is_type(c_page2, "html") && strlen(body(c_page2)) == sizeof page, "another path: the page");
    CHECK(is_type(c_scan, "plain") && !strcmp(body(c_scan), "3 68695C22\n0 \n"), "scan list: \"%s\"", body(c_scan));
    CHECK(is_type(c_cfg, "plain") && !strcmp(body(c_cfg), cfg0), "GET /config.txt: the file");
    CHECK(is_type(c_lang, "plain") && !strcmp(body(c_lang), "1"), "GET /lang.txt: the box language");
    CHECK(is_type(c_version, "plain") && !strcmp(body(c_version), "CFW_9.9.9"), "GET /version.txt: the firmware's version");
    CHECK(!strcmp(body(c_lang_set), "ok") && !strcmp(body(c_lang_bad), "bad") && language == 2,
          "POST /lang.txt: \"2\" stored, \"7\" refused");
    char now_cfg[2000];
    CHECK(!strcmp(body(c_post), "ok") && get("/cfw/config.txt", now_cfg, sizeof now_cfg) == strlen(new_cfg) &&
              !strcmp(now_cfg, new_cfg) && get("/cfw/config.tmp", now_cfg, sizeof now_cfg) == (size_t)-1,
          "POST /config.txt: written, no temporary file left");
    CHECK(!strcmp(body(c_vendor), "vendor") && get("/cfw/config.txt", now_cfg, sizeof now_cfg) == strlen(new_cfg),
          "vendor domain: \"vendor\", file unchanged");
    CHECK(!strcmp(body(c_long), "long"), "1025 bytes: \"long\"");
    CHECK(!strcmp(body(c_cert), "ok") && cert_calls == 1 && cert_i == 2 && cert_len == 300 && !memcmp(cert_got, der, 300),
          "POST /cert/2: 300 bytes in 50-byte pieces to cloud_cert_put(2)");
    CHECK(!strcmp(body(c_cert3), "der") && !strcmp(body(c_big), "bad") && cert_calls == 1,
          "/cert/3: \"der\"; 4097 bytes: \"bad\"; not passed on");
    CHECK(!strcmp(body(c_wifi), "ok") && submits == 1 && creds.ssid_len == 2 && !memcmp(creds.ssid, "N1", 2) &&
              creds.key_len == 8 && !memcmp(creds.key, "secret!!", 8) && creds.mode == 3 && creds.language == 3,
          "POST /wifi: the token fields, one submit");
    CHECK(!strcmp(body(c_bad), "bad") && !strcmp(body(c_huge), "bad"), "malformed, head over 1400 bytes: \"bad\"");
    CHECK(conns[c_silent].out_len == 0, "incomplete request: closed after the pause, no answer");
    CHECK(!strcmp(body(c_rwifi), "ok") && !strcmp(body(c_rcontent), "ok") && !strcmp(body(c_rserver), "ok") &&
              resets_wifi == 1 && resets_content == 1 && resets_server == 1,
          "POST /reset/wifi, /reset/content, /reset/server: each carried out once");
    CHECK(!strcmp(body(c_rescan), "ok") && rescans == 1 && rescan_answered == 1, "POST /rescan: answered, then the new search asked for once");

    printf("P3 certificate answers\n");
    memset(conns, 0, sizeof conns);
    n_conns = next_conn = 0;
    allowed = true;
    chunk = 1400;
    cert_ret = -1;
    int c1 = conn(r_cert, strlen("POST /cert/2 HTTP/1.1\r\nContent-Type: application/octet-stream\r\nContent-Length: 300\r\n\r\n") + 300);
    page_run(work, sizeof work, buf, sizeof buf);
    CHECK(!strcmp(body(c1), "der"), "refused: \"der\"");
    cert_ret = 1;
    memset(conns, 0, sizeof conns);
    n_conns = next_conn = 0;
    allowed = true;
    int c2 = conn("POST /cert/1 HTTP/1.1\r\nContent-Length: 2\r\n\r\n\x30\x00", 46);
    page_run(work, sizeof work, buf, sizeof buf);
    CHECK(c1 == 0 && c2 == 0 && !strcmp(body(0), "flash") && cert_i == 1 && cert_len == 2, "write failed: \"flash\"");
    CHECK(marks > 0, "the activity marked its progress (%d)", marks);

    printf("page test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
