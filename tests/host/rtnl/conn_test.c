/*
 * Host test of the log connection of rtnl.c: drives the log task with a
 * simulated clock, network session
 * and socket, decodes what was sent and checks it.
 * Build and run: tests/host/rtnl/run.sh
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include "rtnl.c"

int test_lock_held;
static uint32_t now_ms = 1000;
static int loops_left;
static jmp_buf jb;
static uint32_t sess = 1;
static uint8_t st = 6;
static int connect_result = 5, connects, closes;
static uint32_t connect_times[64];
static const uint8_t *rx;
static int rxlen, rx_zero;
static int send_fail, send_with_lock;
static uint8_t wire[200000];
static int wlen;
static uint8_t uart[200000];
static int ulen;
static int failures;

#define CHECK(c, ...)                                   \
    do                                                  \
    {                                                   \
        if (!(c))                                       \
        {                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            failures++;                                 \
        }                                               \
    } while (0)

uint32_t tick_ms(void) { return now_ms; }
uint32_t clock_now(void) { return 1791776107; }
void vTaskDelay(TickType_t t)
{
    now_ms += t;
    if (--loops_left <= 0)
    {
        longjmp(jb, 1);
    }
}
TaskHandle_t xTaskCreateStatic(void (*f)(void *), const char *n, uint32_t d, void *p, UBaseType_t pr,
                               StackType_t *s, StaticTask_t *tc)
{
    (void)f;
    (void)n;
    (void)d;
    (void)p;
    (void)pr;
    (void)s;
    (void)tc;
    return (TaskHandle_t)1;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
uint8_t buildblock_slot(void) { return 2; }
void monitor_mark(activity_t a) { (void)a; }
void debug_printf(const char *f, ...)
{
    if (getenv("VERBOSE"))
    {
        va_list ap;
        va_start(ap, f);
        printf("  [%u] ", now_ms);
        vprintf(f, ap);
        va_end(ap);
    }
}
void debug_write_bin(const void *p, unsigned n)
{
    memcpy(uart + ulen, p, n);
    ulen += (int)n;
}
uint32_t net_session(void) { return sess; }
uint8_t net_state(void) { return st; }
int16_t cloud_log_connect(uint32_t s)
{
    (void)s;
    connect_times[connects++] = now_ms;
    return (int16_t)connect_result;
}
void cloud_sync_request(bool u) { (void)u; }
void cloud_log_closed(void) {}
bool cloud_log_yield_wanted(void) { return false; }
int16_t sl_Send(int16_t s, const void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)f;
    if (test_lock_held)
    {
        send_with_lock++;
    }
    if (send_fail)
    {
        return -1;
    }
    memcpy(wire + wlen, b, (size_t)n);
    wlen += n;
    return n;
}
static int rx_chunk; /* > 0: at most this many bytes per receive */
static bool consume; /* bytes not read stay for the next receive (else dropped, as a 16-byte read) */
int16_t sl_Recv(int16_t s, void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)f;
    if (rx_zero)
    {
        rx_zero = 0;
        return 0;
    }
    if (!rxlen)
    {
        return SL_EAGAIN;
    }
    int k = rxlen < n ? rxlen : n;
    if (rx_chunk && k > rx_chunk)
    {
        k = rx_chunk;
    }
    memcpy(b, rx, (size_t)k);
    if (consume)
    {
        rx += k;
        rxlen -= k;
    }
    else
    {
        rxlen = 0;
    }
    return (int16_t)k;
}

/* F12 stubs */
static bool hello;
static int remote_calls, remote_last_c, remote_last_n, remote_changed_flag, status_calls;
bool ext_hello_received(void) { return hello; }
static int hello_calls;
void ext_hello(void) { hello_calls++; }
uint8_t remote_command(uint8_t c, const uint8_t *arg, unsigned n)
{
    (void)arg;
    remote_calls++;
    remote_last_c = c;
    remote_last_n = (int)n;
    return c == 66 ? 1 : 0;
}
void remote_status(remote_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->state = 2;
    st->chapter = 2;
    st->volume = 6;
    status_calls++;
    remote_changed_flag = 0;
}
bool remote_changed(void) { return remote_changed_flag; }
int16_t sl_Close(int16_t s)
{
    (void)s;
    closes++;
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

static void run(int n)
{
    loops_left = n;
    if (!setjmp(jb))
    {
        task(NULL);
    }
}

/* --- decoding --- */

typedef struct
{
    int kind; /* 2 function, 3 event */
    uint64_t f[11];
    bool has[11];
    unsigned args_len;
    uint8_t args[80];
} rec_t;

static unsigned dvar(const uint8_t *p, uint64_t *v)
{
    unsigned i = 0;
    *v = 0;
    do
    {
        *v |= (uint64_t)(p[i] & 0x7F) << (7 * i);
    } while (p[i++] & 0x80);
    return i;
}

static bool decode(const uint8_t *m, unsigned len, rec_t *r)
{
    memset(r, 0, sizeof *r);
    uint64_t k, l;
    unsigned i = dvar(m, &k);
    r->kind = (int)(k >> 3);
    i += dvar(m + i, &l);
    if (i + l != len)
    {
        return false;
    }
    const uint8_t *p = m + i, *end = p + l;
    unsigned last = 0;
    while (p < end)
    {
        p += dvar(p, &k);
        unsigned fn = (unsigned)(k >> 3), wt = k & 7;
        if (fn <= last || fn > 10)
        {
            return false; /* ascending, once */
        }
        last = fn;
        uint64_t v = 0;
        if (wt == 0)
        {
            p += dvar(p, &v);
        }
        else if (wt == 1)
        {
            memcpy(&v, p, 8);
            p += 8;
        }
        else if (wt == 5)
        {
            uint32_t w;
            memcpy(&w, p, 4);
            v = w;
            p += 4;
        }
        else if (wt == 2)
        {
            uint64_t l2;
            p += dvar(p, &l2);
            v = l2;
            if (fn == 6)
            {
                r->args_len = (unsigned)l2;
                memcpy(r->args, p, l2 < sizeof r->args ? l2 : sizeof r->args);
                v = l2 >= 4 ? (uint64_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) : 0;
            }
            p += l2;
        }
        r->f[fn] = v;
        r->has[fn] = true;
    }
    return p == end;
}

/* Walks a stream frame by frame; returns the number of
 * records, -1 if it does not walk. */
static int walk(const uint8_t *s, int n, rec_t *out, int max)
{
    int i = 0, c = 0;
    while (i < n)
    {
        if (n - i < 4 || s[i] || s[i + 1])
        {
            return -1;
        }
        unsigned len = (unsigned)s[i + 2] << 8 | s[i + 3];
        if (len < 1 || len > 176 || i + 4 + (int)len > n)
        {
            return -1;
        }
        if (c < max && !decode(s + i + 4, len, &out[c]))
        {
            return -1;
        }
        c++;
        i += 4 + (int)len;
    }
    return c;
}

static rec_t recs[4000];

static int find_fn(int n, unsigned group, unsigned fn)
{
    for (int i = 0; i < n; i++)
    {
        if (recs[i].kind == 2 && recs[i].f[4] == group && recs[i].f[5] == fn)
        {
            return i;
        }
    }
    return -1;
}

static int first_fn(int n)
{
    for (int i = 0; i < n; i++)
    {
        if (recs[i].kind == 2)
        {
            return i;
        }
    }
    return -1;
}

/* Records with fields 8/9 other than group 8 or 28. */
static int stray_identity(int n, int except)
{
    int c = 0;
    for (int i = 0; i < n; i++)
    {
        if (i != except && recs[i].kind == 2 && recs[i].has[8] && recs[i].f[4] != 8 && recs[i].f[4] != 28)
        {
            c++;
        }
    }
    return c;
}

static void cmd(const uint8_t *f, int n)
{
    rx = f;
    rxlen = n;
}

int main(void)
{
    log_init();
    log_start(0x0A);
    round_session = 1;

    /* 1: first connection */
    run(3);
    CHECK(connects == 1 && sock >= 0, "first connection: %d connects", connects);
    run(3);
    int n = walk(wire, wlen, recs, 4000);
    CHECK(n > 0, "stream walks");
    int f = first_fn(n);
    CHECK(f >= 0 && recs[f].has[8] && recs[f].has[9] && recs[f].f[8] == CFW_BUILD_TIME,
          "first function record sent carries fields 8 and 9");
    int c = find_fn(n, 37, 7548);
    CHECK(c >= 0 && recs[c].f[6] == 5, "connection record with the socket number");
    CHECK(stray_identity(n, f) == 0, "no other record with fields 8/9");
    CHECK(send_with_lock == 0, "sl_Send without the buffer lock (%d)", send_with_lock);
    int u = walk(uart, ulen, recs, 4000);
    CHECK(u > 0, "UART copy walks as frames");
    wlen = 0;
    ulen = 0;

    /* 2: host closes: next attempt about 2 s later */
    run(20);
    rx_zero = 1;
    int c0 = connects;
    uint32_t t0 = now_ms;
    run(80);
    CHECK(connects == c0 + 1 && connect_times[c0] - t0 >= 1900 && connect_times[c0] - t0 <= 3100,
          "reconnect after a host close %d ms later", connects > c0 ? (int)(connect_times[c0] - t0) : -1);
    wlen = 0;

    /* 3: command 2 with sequence 0: pause 1000 ms
     * (MAC ends in 0x0A) plus 2000 ms */
    run(30);
    static const uint8_t cmd2[] = {0, 0, 0, 8, 0x08, 2, 0x10, 0};
    cmd(cmd2, sizeof cmd2);
    c0 = connects;
    t0 = now_ms;
    run(200);
    CHECK(connects == c0 + 1 && connect_times[c0] - t0 >= 2900 && connect_times[c0] - t0 <= 4100,
          "after command 2 next connect %d ms later", connects > c0 ? (int)(connect_times[c0] - t0) : -1);
    n = walk(wire, wlen, recs, 4000);
    c = find_fn(n, 37, 6733);
    CHECK(c >= 0 && recs[c].has[10] && recs[c].f[10] == 0 && recs[c].f[3] == 1, "ack 6733 with field 10 = 0");
    c = find_fn(n, 37, 6734);
    CHECK(c >= 0 && recs[c].f[6] == 1000, "pause record 6734 with 1000 ms");
    u = walk(uart, ulen, recs, 4000);
    CHECK(u >= 0 && find_fn(u, 37, 6733) < 0 && find_fn(u, 37, 6734) >= 0, "ack not on the UART, 6734 is");
    wlen = 0;

    /* 4: command frames */
    static const struct
    {
        uint8_t f[16];
        int n;
        bool keep;
        const char *what;
    } frames[] = {
        {{0, 0, 0, 6, 0x08, 1}, 6, true, "command 1 without sequence"},
        {{0, 0, 0, 6, 0x08, 0x81}, 6, false, "truncated varint"},
        {{0, 0, 0, 9, 0x08, 1, 0x1A, 1, 0x41}, 9, true, "unknown length-delimited field"},
        {{0, 0, 0, 11, 0x08, 1, 0x25, 1, 2, 3, 4}, 11, true, "unknown fixed32 field"},
        {{0, 0, 0, 15, 0x08, 1, 0x21, 1, 2, 3, 4, 5, 6, 7, 8}, 15, true, "unknown fixed64 field"},
        {{0, 0, 0, 9, 0x1A, 3, 0x41}, 7, false, "length beyond the frame"},
        {{0, 0, 0, 15, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 15, false, "key varint of 11 bytes"},
        {{0, 0, 0, 15, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02}, 15, false, "varint above 2^64 - 1"},
        {{0, 0, 0, 15, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 15, true, "command -1 as a 10-byte varint"},
        {{0, 0, 0, 7, 0x0A, 1, 0x00}, 7, false, "field 1 with wire type 2"},
        {{0, 0, 0, 6, 0x10, 0}, 6, false, "no field 1"},
        {{0, 0, 0, 7, 0x08, 0x80, 0x01}, 7, false, "command 128 out of range"},
        {{0, 0, 0, 6, 0x00, 0}, 6, false, "field number 0"},
    };
    for (unsigned i = 0; i < sizeof frames / sizeof frames[0]; i++)
    {
        run(40); /* connected again */
        CHECK(sock >= 0, "connected before frame %u", i);
        cmd(frames[i].f, frames[i].n);
        int cl = closes;
        run(25);
        CHECK((closes == cl) == frames[i].keep, "%s: connection %s", frames[i].what, closes == cl ? "kept" : "closed");
    }

    /* command 63: the hello of the log host */
    run(40);
    wlen = 0;
    static const uint8_t hello63[] = {0, 0, 0, 6, 0x08, 0x3F};
    cmd(hello63, sizeof hello63);
    int cl63 = closes;
    run(25);
    n = walk(wire, wlen, recs, 4000);
    CHECK(hello_calls == 1 && closes == cl63 && find_fn(n, 37, 6739) >= 0,
          "command 63: the hello, acknowledged as an unknown command, connection kept");
    wlen = 0;

    /* 5: records buffered while disconnected; the first function frame in
     * the buffer gets fields 8/9 when the connection opens */
    run(80);
    sess = 2; /* Wi-Fi change */
    run(2);
    CHECK(sock < 0, "closed on a Wi-Fi change");
    round_session = 0;
    log_event(EVT_BIG_EAR, NULL);
    log_args_t a = {0};
    for (int i = 0; i < 18; i++)
    {
        log_arg_num(&a, i);
    }
    log_arg_text(&a, "abcdefghij"); /* cut to 7 characters: 80 bytes */
    log_fn(LOG_WARN, 24, 1234, 1, &a);
    log_fn(LOG_INFO, 24, 1235, 0, NULL);
    run(5);
    wlen = 0;
    round_session = 2;
    run(60);
    n = walk(wire, wlen, recs, 4000);
    CHECK(n >= 3 && recs[0].kind == 3, "event record first (%d)", n);
    if (getenv("VERBOSE"))
    {
        for (int i = 0; i < n && i < 4; i++)
        {
            printf("rec %d kind %d fn %lu has8 %d has7 %d lvl %lu args %u\n", i, recs[i].kind, (unsigned long)recs[i].f[5],
                   recs[i].has[8], recs[i].has[7], (unsigned long)recs[i].f[3], recs[i].args_len);
        }
    }
    CHECK(n >= 3 && recs[1].kind == 2 && recs[1].f[5] == 1234 && recs[1].has[8] && recs[1].has[7] &&
              recs[1].f[3] == 3 && recs[1].args_len == 80,
          "buffered function record got fields 8/9, other fields unchanged");
    CHECK(stray_identity(n, 1) == 0, "only that record");
    wlen = 0;

    /* 6: buffer overflow while disconnected: trimming and the
     * discard count */
    sess = 3;
    run(2);
    round_session = 0;
    uint32_t seq0 = sequence;
    a.n = 0;
    log_arg_text(&a, "0123456789012345678901234567890123456789");
    for (int i = 0; i < 200; i++)
    {
        log_fn(LOG_INFO, 24, 2000, 0, &a);
    }
    uint32_t d = discards;
    CHECK(d > 0, "records discarded when full (%lu)", (unsigned long)d);
    run(30); /* > 1 s without a connection */
    CHECK(used * 100 / BUF_SIZE <= 50, "trimmed to %u %%", used * 100 / BUF_SIZE);
    round_session = 3;
    wlen = 0;
    run(60);
    n = walk(wire, wlen, recs, 4000);
    c = find_fn(n, 37, 7548);
    CHECK(c >= 0 && recs[c].f[6] == 5, "connection record");
    uint32_t d3 = 0;
    if (c >= 0)
    {
        memcpy(&d3, recs[c].args + 8, 4);
    }
    CHECK(d3 == d, "connection record reports %lu discards (%lu)", (unsigned long)d3, (unsigned long)d);
    CHECK(n > 0 && recs[0].f[2] > seq0 + 1, "oldest frames removed (first sequence %lu)", (unsigned long)recs[0].f[2]);
    CHECK(discards == 0, "discard counter reset");

    /* 7: a failed send in the acknowledgement drain closes the connection */
    run(30);
    static const uint8_t cmd1s[] = {0, 0, 0, 8, 0x08, 1, 0x10, 0};
    cmd(cmd1s, sizeof cmd1s);
    send_fail = 1;
    int cl = closes;
    run(25);
    CHECK(closes == cl + 1 && sock < 0, "failed send in the drain closes");
    send_fail = 0;
    CHECK(send_with_lock == 0, "sl_Send never under the buffer lock");

    /* 8: the insertion when the record's length varint grows from 1 to 2
     * bytes (a crafted record of 126 bytes) */
    {
        uint8_t m[140];
        unsigned k = 0;
        m[k++] = 0x12;
        m[k++] = 126;
        m[k++] = 0x10;
        m[k++] = 7;
        m[k++] = 0x32;
        m[k++] = 122;
        memset(m + k, 0xAA, 122);
        k += 122;
        test_lock_held = 0;
        used = 0;
        append(m, k);
        identity_to_first();
        rec_t r;
        unsigned len = (unsigned)buf[2] << 8 | buf[3];
        CHECK(identity_pending == false && decode(buf + 4, len, &r) && r.has[8] && r.has[9] && r.f[2] == 7 &&
                  r.args_len == 122 && used == 4 + len && len == k + 1 + 7 + strlen(CFW_REVISION),
              "insertion with a growing length varint (len %u)", len);
    }

    /* 9: F12 remote command frames */
    consume = true;
    send_fail = 0;
    run(60);
    CHECK(sock >= 0, "connected for F12");
    static uint8_t f40[40] = {0, 0, 0, 40, 0x08, 72, 0x10, 0, 0x1A, 30};
    cmd(f40, 40);
    int cl9 = closes;
    run(25);
    CHECK(closes == cl9 + 1, "40-byte frame before the hello closes");
    rxlen = 0; /* the rest of that frame is not read again */
    hello = true;
    run(60);
    expected_cmd = 0;
    wlen = 0;
    cmd(f40, 40);
    rx_chunk = 7; /* the frame arrives in pieces */
    cl9 = closes;
    run(25);
    rx_chunk = 0;
    CHECK(closes == cl9, "40-byte frame after the hello kept");
    CHECK(remote_calls == 1 && remote_last_c == 72 && remote_last_n == 30, "command 72 with 30 argument bytes");
    n = walk(wire, wlen, recs, 4000);
    int ack = find_fn(n, 37, 6739), res = find_fn(n, 22, 32260), stt = find_fn(n, 22, 32261);
    CHECK(ack >= 0 && recs[ack].has[10] && recs[ack].f[10] == 0, "acknowledged with 6739, field 10 = 0");
    CHECK(res > ack && recs[res].f[6] == 72 && recs[res].args_len == 12, "result record");
    CHECK(stt > res && recs[stt].f[6] == 2 && recs[stt].args_len == 36, "status record after the result");
    /* command 66 with sequence 1, result 1 */
    static const uint8_t c66[] = {0, 0, 0, 8, 0x08, 66, 0x10, 1};
    cmd(c66, sizeof c66);
    wlen = 0;
    run(25);
    n = walk(wire, wlen, recs, 4000);
    res = find_fn(n, 22, 32260);
    uint32_t r3[3] = {0};
    if (res >= 0)
    {
        memcpy(r3, recs[res].args, 12);
    }
    CHECK(res >= 0 && r3[0] == 66 && r3[1] == 1 && r3[2] == 1, "result 66, 1, 1");
    CHECK(find_fn(n, 22, 32261) < 0, "no status record for 66");
    /* status on change, at most once per second */
    wlen = 0;
    int sc = status_calls;
    for (int i = 0; i < 10; i++)
    {
        remote_changed_flag = 1;
        run(4); /* 200 ms */
    }
    CHECK(status_calls - sc >= 1 && status_calls - sc <= 3, "status records on change: %d in 2 s", status_calls - sc);
    /* field 3 above 300 bytes: invalid */
    static uint8_t big[320] = {0, 0, 1, 64, 0x08, 64, 0x1A, 0xAD, 0x02}; /* field 3 of 301 bytes */
    cmd(big, 320);
    cl9 = closes;
    run(25);
    CHECK(closes == cl9 + 1, "arguments above 300 bytes close");
    /* a frame above 320 bytes: invalid */
    run(60);
    static const uint8_t huge[] = {0, 0, 1, 65};
    cmd(huge, 4);
    cl9 = closes;
    run(25);
    CHECK(closes == cl9 + 1, "321-byte frame closes");
    /* new connection: no status on change until a remote command */
    run(60);
    sc = status_calls;
    remote_changed_flag = 1;
    run(40);
    CHECK(status_calls == sc, "no status records on a connection without remote commands");
    /* a short frame still works after the hello */
    static const uint8_t c1[] = {0, 0, 0, 6, 0x08, 1};
    cmd(c1, sizeof c1);
    cl9 = closes;
    run(25);
    CHECK(closes == cl9, "6-byte frame after the hello");

    printf("rtnl connection test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
