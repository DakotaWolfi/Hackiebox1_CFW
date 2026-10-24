/*
 * Log stream and the frames and records of the
 * F12 remote control (the actions
 * are in remote.c). Not yet: the fault history, the Wi-Fi receive statistics
 * of attempts during an idle
 * power-off, the actions of commands 3 to 5, and most of the catalogue.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "port.h"
#include "buildblock.h"
#include "clock.h"
#include "cloud.h"
#include "debug.h"
#include "ext.h"
#include "net.h"
#include "remote.h"
#include "lowram.h"
#include "monitor.h"
#include "rtnl.h"
#include "sock_port.h"
#include "tick.h"
#include "version.h"

/* Raw bytes to the debug serial port, without the line-end
 * translation of debug_write; must work in an interrupt handler too. Weak
 * until debug.c provides it: without it nothing is copied. */
void debug_write_bin(const void *p, unsigned n) __attribute__((weak));
/* cloud.c; declared here until cloud.h has it. */
void cloud_log_closed(void);

static bool in_isr(void)
{
#ifdef __arm__
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) != 0;
#else
    return false;
#endif
}

/* --- payload --- */

void log_arg_num(log_args_t *a, int32_t v)
{
    if (a->n >= 76)
    {
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        a->data[a->n++] = (uint8_t)((uint32_t)v >> (8 * i));
    }
}

void log_arg_text(log_args_t *a, const char *s)
{
    if (a->n >= 76)
    {
        return;
    }
    if (!s)
    {
        s = "(null)";
    }
    while (*s && a->n < 79)
    {
        a->data[a->n++] = (uint8_t)*s++;
    }
    a->data[a->n++] = 0;
}

/* --- encoding --- */

static unsigned put_varint(uint8_t *p, uint32_t v)
{
    unsigned n = 0;
    do
    {
        uint8_t b = v & 0x7F;
        v >>= 7;
        p[n++] = v ? (b | 0x80) : b;
    } while (v);
    return n;
}

static unsigned put_fixed(uint8_t *p, uint32_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
    {
        p[i] = i < 4 ? (uint8_t)(v >> (8 * i)) : 0;
    }
    return bytes;
}

/* Fields 8 and 9, 7 bytes plus the revision. */
static unsigned put_identity(uint8_t *p)
{
    unsigned n = 0;
    p[n++] = 0x45;
    n += put_fixed(p + n, CFW_BUILD_TIME, 4);
    p[n++] = 0x4A;
    size_t rl = strlen(CFW_REVISION);
    p[n++] = (uint8_t)rl;
    memcpy(p + n, CFW_REVISION, rl);
    return n + rl;
}

/* Bytes of the varint at p, 0 when it is truncated or longer than 10
 * bytes or above 2^64 - 1. */
static unsigned get_varint(const uint8_t *p, const uint8_t *end, uint64_t *v)
{
    uint64_t w = 0;
    for (unsigned i = 0; i < 10 && p + i < end; i++)
    {
        uint8_t b = p[i];
        if (i == 9 && b > 1)
        {
            return 0;
        }
        w |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80))
        {
            *v = w;
            return i + 1;
        }
    }
    return 0;
}

/* Skips the value of a field of wire type wt; NULL when it does not
 * decode. */
static const uint8_t *skip_value(const uint8_t *p, const uint8_t *end, unsigned wt)
{
    uint64_t v;
    unsigned k;
    switch (wt)
    {
    case 0:
        k = get_varint(p, end, &v);
        return k ? p + k : NULL;
    case 1:
        return end - p >= 8 ? p + 8 : NULL;
    case 5:
        return end - p >= 4 ? p + 4 : NULL;
    case 2:
        k = get_varint(p, end, &v);
        return k && v <= (uint64_t)(end - p - k) ? p + k + v : NULL;
    default:
        return NULL;
    }
}

/* --- record buffer --- */

#define BUF_SIZE 6916u
static uint8_t buf[BUF_SIZE] LOWRAM;
static unsigned used; /* frames from the start of buf */
static uint32_t sequence, discards;
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;
static bool identity_pending; /* the next function record gets fields 8 and 9 */

/* Frames are appended by any task; only the log task removes frames from
 * the front, trims the buffer and edits frames, so the frames
 * it is sending stay in place without the lock. */
static void append(const uint8_t *msg, unsigned len)
{
    if (used + 4 + len > BUF_SIZE)
    {
        discards++;
        return;
    }
    buf[used] = buf[used + 1] = 0;
    buf[used + 2] = (uint8_t)(len >> 8);
    buf[used + 3] = (uint8_t)len;
    memcpy(buf + used + 4, msg, len);
    used += 4 + len;
}

static void remove_front(unsigned n)
{
    memmove(buf, buf + n, used - n);
    used -= n;
}

static unsigned frame_len(unsigned at)
{
    return 4u + ((unsigned)buf[at + 2] << 8 | buf[at + 3]);
}

/* Catalogue rows marked "no UART"; the command
 * acknowledgements are passed as such by their caller. */
static bool uart_copy(uint16_t group, uint16_t function)
{
    static const uint16_t no[][2] = {{6, 8656}, {15, 8510}, {15, 8715}, {22, 8699}, {22, 10071}, {22, 10077}, {22, 10078}};
    for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++)
    {
        if (no[i][0] == group && no[i][1] == function)
        {
            return false;
        }
    }
    return true;
}

static void fn_record(uint8_t level, uint16_t group, uint16_t function, uint8_t variant, const log_args_t *a,
                      bool has_cmd, uint32_t cmd, bool uart)
{
    uint8_t rec[168], msg[4 + 176];
    unsigned n = 0;
    bool isr = in_isr();
    if (!lock)
    {
        return;
    }
    if (!isr)
    {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    rec[n++] = 0x09;
    n += put_fixed(rec + n, tick_ms(), 8);
    rec[n++] = 0x10;
    uint32_t seq;
    if (isr)
    {
        UBaseType_t m = taskENTER_CRITICAL_FROM_ISR();
        seq = ++sequence;
        taskEXIT_CRITICAL_FROM_ISR(m);
    }
    else
    {
        CRITICAL_ENTER();
        seq = ++sequence;
        CRITICAL_EXIT();
    }
    n += put_varint(rec + n, seq);
    rec[n++] = 0x18;
    n += put_varint(rec + n, level);
    rec[n++] = 0x20;
    n += put_varint(rec + n, group);
    rec[n++] = 0x28;
    n += put_varint(rec + n, function);
    rec[n++] = 0x32;
    unsigned an = a ? a->n : 0;
    n += put_varint(rec + n, an);
    if (an)
    {
        memcpy(rec + n, a->data, an);
    }
    n += an;
    if (variant)
    {
        rec[n++] = 0x38;
        n += put_varint(rec + n, variant);
    }
    /* A record made in an interrupt handler never reaches the
     * log host, so it does not take the place of the first one. */
    if (group == 8 || group == 28 || (identity_pending && !isr))
    {
        if (!isr)
        {
            identity_pending = false;
        }
        n += put_identity(rec + n);
    }
    if (has_cmd)
    {
        rec[n++] = 0x50;
        n += put_varint(rec + n, cmd);
    }
    unsigned m = 4;
    msg[m++] = 0x12;
    m += put_varint(msg + m, n);
    memcpy(msg + m, rec, n);
    m += n;
    msg[0] = msg[1] = 0;
    msg[2] = (uint8_t)((m - 4) >> 8);
    msg[3] = (uint8_t)(m - 4);
    /* not into the buffer from an interrupt handler. */
    if (!isr)
    {
        append(msg + 4, m - 4);
        xSemaphoreGive(lock);
    }
    if (uart && debug_write_bin)
    {
        debug_write_bin(msg, m);
    }
}

void log_fn(uint8_t level, uint16_t group, uint16_t function, uint8_t variant, const log_args_t *a)
{
    fn_record(level, group, function, variant, a, false, 0, uart_copy(group, function));
}

void log_event(log_event_t type, const uint8_t *uid)
{
    uint8_t msg[32];
    unsigned n = 0;
    uint32_t t = clock_now();
    if (!lock || t < CFW_BUILD_TIME || in_isr())
    {
        return; /* clock base not set yet */
    }
    msg[n++] = 0x1A;
    unsigned len_at = n++;
    msg[n++] = 0x0D;
    n += put_fixed(msg + n, t, 4);
    msg[n++] = 0x10;
    n += put_varint(msg + n, type);
    if (uid)
    {
        msg[n++] = 0x1A;
        msg[n++] = 8;
        memcpy(msg + n, uid, 8);
        n += 8;
    }
    msg[len_at] = (uint8_t)(n - len_at - 1);
    xSemaphoreTake(lock, portMAX_DELAY);
    append(msg, n);
    xSemaphoreGive(lock);
}

/* On a new connection: the first function frame waiting
 * in the buffer gets fields 8 and 9 (unless it has them); without one, or
 * without room for them, the next function record generated gets them. */
static void identity_to_first(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    identity_pending = true;
    for (unsigned at = 0; at < used; at += frame_len(at))
    {
        unsigned flen = frame_len(at) - 4;
        const uint8_t *m = buf + at + 4, *mend = m + flen;
        if (m[0] != 0x12)
        {
            continue; /* an event record */
        }
        uint64_t rl;
        unsigned lb = get_varint(m + 1, mend, &rl);
        if (!lb || rl > (uint64_t)(mend - m - 1 - lb))
        {
            break;
        }
        const uint8_t *r = m + 1 + lb, *rend = r + rl, *q = r;
        bool has = false;
        while (q < rend)
        {
            uint64_t key;
            unsigned kb = get_varint(q, rend, &key);
            if (!kb)
            {
                break;
            }
            if ((key >> 3) >= 8)
            {
                has = (key >> 3) <= 9;
                break;
            }
            q = skip_value(q + kb, rend, key & 7);
            if (!q)
            {
                break;
            }
        }
        if (!q)
        {
            break;
        }
        if (!has)
        {
            uint8_t tmp[176 + 16];
            unsigned d = 0;
            tmp[d++] = 0x12;
            d += put_varint(tmp + d, (uint32_t)rl + 7 + (uint32_t)strlen(CFW_REVISION));
            memcpy(tmp + d, r, (size_t)(q - r));
            d += (unsigned)(q - r);
            d += put_identity(tmp + d);
            memcpy(tmp + d, q, (size_t)(rend - q));
            d += (unsigned)(rend - q);
            unsigned grow = d - flen;
            if (d > 176 || used + grow > BUF_SIZE)
            {
                break;
            }
            unsigned tail = at + 4 + flen;
            memmove(buf + tail + grow, buf + tail, used - tail);
            memcpy(buf + at + 4, tmp, d);
            buf[at + 2] = (uint8_t)(d >> 8);
            buf[at + 3] = (uint8_t)d;
            used += grow;
        }
        identity_pending = false;
        break;
    }
    xSemaphoreGive(lock);
}

/* --- identity records --- */

static void identity(void)
{
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)CFW_BUILD_TIME);
    log_arg_text(&a, "FW");
    log_arg_text(&a, "");
    log_arg_text(&a, CFW_REVISION);
    log_arg_text(&a, CFW_BUILD_DATE);
    log_arg_num(&a, 9);
    log_arg_num(&a, 3);
    log_arg_num(&a, 1);
    log_arg_num(&a, buildblock_slot());
    log_fn(LOG_INFO, 8, 7146, 0, &a);
    a.n = 0;
    log_arg_text(&a, CFW_VERSION_STRING);
    log_fn(LOG_INFO, 8, 703, 0, &a);
    a.n = 0;
    log_arg_text(&a, CFW_BRANCH_STRING);
    log_fn(LOG_INFO, 8, 704, 0, &a);
    /* The hardware description has no layout yet. */
    a.n = 0;
    log_arg_num(&a, 0);
    log_arg_num(&a, 1);
    log_arg_num(&a, 0);
    log_arg_num(&a, 0);
    log_fn(LOG_INFO, 4, 7305, 0, &a);
}

/* --- connection --- */

static volatile uint32_t round_session;
static volatile int16_t sock = -1;
static uint32_t attempts, expected_cmd, next_attempt;
static volatile uint32_t conn_session; /* network session of the open connection */
static uint32_t mac_last;
static TaskHandle_t log_task;
static bool remote_on; /* a remote command on this connection */
static uint32_t status_at;
static bool status_due;
static long last_err; /* driver result of the last failed send or receive */

/* F12 frames and commands after the hello of this start. */
static bool remote_enabled(void)
{
    return (EXT_CAPS & EXT_CAP_F12_REMOTE) && ext_hello_received();
}

void log_round_done(uint32_t s)
{
    round_session = s;
}

bool log_connected(void)
{
    return sock >= 0;
}

/* The count of failed attempts restarts and the next attempt
 * waits 1000 + 1000 * 2^0 ms. */
static void close_log(const char *why)
{
    if (sock >= 0)
    {
        sock_close(sock);
        debug_printf("rtnl: log closed (%s, %ld)\n", why, last_err);
        sock = -1;
        cloud_log_closed();
    }
    sock = -1;
    attempts = 0;
    next_attempt = tick_ms() + 2000;
}

/* A Wi-Fi change is pending or has been. */
static bool wifi_changed(void)
{
    return net_session() != conn_session;
}

/* Sends whole frames, at most 1386 bytes; false on a failed
 * send. The lock is not held while sending (records can still
 * be made while the log host is slow). */
static bool send_some(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned n = 0;
    while (n < used && n + frame_len(n) <= 1386)
    {
        n += frame_len(n);
    }
    xSemaphoreGive(lock);
    if (!n)
    {
        return true;
    }
    long r = sock_send(sock, buf, n);
    if (r != (long)n)
    {
        last_err = r;
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    remove_front(n);
    xSemaphoreGive(lock);
    return true;
}

static bool buffer_empty(void)
{
    return used == 0;
}

void log_drain(void)
{
    uint32_t s = conn_session;
    if (sock < 0 || net_session() != s || in_isr() || xTaskGetCurrentTaskHandle() == log_task)
    {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (buffer_empty())
    {
        return;
    }
    for (int i = 0; i < 50 && !buffer_empty() && sock >= 0 && net_session() == s; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

/* The message of a command frame: field 1 int32 required,
 * field 2 uint32 optional, other fields skipped by their wire type. After
 * the hello (ext) field 3 carries the arguments of a remote command,
 * length-delimited, at most 300 bytes. */
typedef struct
{
    int32_t cmd;
    bool have_seq;
    uint32_t seq;
    const uint8_t *arg;
    unsigned arg_len;
} command_t;

static bool parse_command(const uint8_t *p, const uint8_t *end, bool ext, command_t *c)
{
    bool have_cmd = false;
    c->have_seq = false;
    c->arg = NULL;
    c->arg_len = 0;
    while (p < end)
    {
        uint64_t key, v;
        unsigned k = get_varint(p, end, &key);
        if (!k || !(key >> 3))
        {
            return false;
        }
        p += k;
        unsigned wt = key & 7;
        uint64_t field = key >> 3;
        if (field == 1 || field == 2)
        {
            if (wt != 0 || !(k = get_varint(p, end, &v)))
            {
                return false;
            }
            p += k;
            if (field == 1)
            {
                int32_t x = (int32_t)(uint32_t)v;
                if (x < -128 || x > 127)
                {
                    return false;
                }
                c->cmd = x;
                have_cmd = true;
            }
            else
            {
                if (v > 0xFFFFFFFFu)
                {
                    return false;
                }
                c->seq = (uint32_t)v;
                c->have_seq = true;
            }
        }
        else if (field == 3 && ext)
        {
            if (wt != 2 || !(k = get_varint(p, end, &v)) || v > 300 || v > (uint64_t)(end - p - k))
            {
                return false;
            }
            c->arg = p + k;
            c->arg_len = (unsigned)v;
            p += k + v;
        }
        else if (!(p = skip_value(p, end, wt)))
        {
            return false;
        }
    }
    return have_cmd;
}

/* Reads exactly n bytes, waiting at most about 1 s for the rest of a frame;
 * the count read, or a negative value / 0 as sock_recv(). */
static long recv_all(uint8_t *in, unsigned n)
{
    unsigned got = 0;
    for (unsigned tries = 0; got < n;)
    {
        long r = sock_recv(sock, in + got, n - got);
        if (r == SOCK_AGAIN)
        {
            if (!got)
            {
                return SOCK_AGAIN;
            }
            if (++tries > 100)
            {
                return -1;
            }
            monitor_mark(ACT_LOG);
            continue;
        }
        if (r <= 0)
        {
            return r;
        }
        got += (unsigned)r;
    }
    return (long)got;
}

/* --- F12 remote control --- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void status_record(void)
{
    remote_status_t st;
    remote_status(&st);
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)st.state);
    log_arg_num(&a, (int32_t)le32(st.uid));
    log_arg_num(&a, (int32_t)le32(st.uid + 4));
    log_arg_num(&a, (int32_t)st.audio_id);
    log_arg_num(&a, (int32_t)st.chapter);
    log_arg_num(&a, (int32_t)st.position);
    log_arg_num(&a, (int32_t)st.volume);
    log_arg_num(&a, (int32_t)st.battery);
    log_arg_num(&a, (int32_t)st.locks);
    log_fn(LOG_INFO, 22, 32261, 0, &a);
    status_at = tick_ms();
    status_due = false;
}

static void remote(uint8_t c, uint32_t seq, const uint8_t *arg, unsigned n)
{
    uint8_t res = remote_command(c, arg, n);
    log_args_t a = {0};
    log_arg_num(&a, c);
    log_arg_num(&a, (int32_t)seq);
    log_arg_num(&a, res);
    log_fn(LOG_INFO, 22, 32260, 0, &a);
    debug_printf("rtnl: remote command %u, result %u\n", c, res);
    if (c == 72)
    {
        status_record();
    }
    remote_on = true;
}

/* at most once per second, the last change of a second at
 * its end. */
static void status_on_change(void)
{
    if (!remote_on)
    {
        return;
    }
    if (!status_due && remote_changed())
    {
        status_due = true;
    }
    if (status_due && tick_ms() - status_at >= 1000)
    {
        status_record();
    }
}

static void commands(void)
{
    /* On the stack, only during the check: frames of up to 320 bytes after
     * the hello, 16 bytes at a time before it. */
    uint8_t in[320];
    bool ext = remote_enabled();
    long r = ext ? recv_all(in, 4) : sock_recv(sock, in, 16);
    if (r == SOCK_AGAIN)
    {
        return;
    }
    if (r <= 0)
    {
        last_err = r;
        close_log(r == 0 ? "by host" : "recv");
        return;
    }
    uint32_t len = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
    if (ext && len >= 5 && len <= sizeof in)
    {
        r = recv_all(in + 4, len - 4);
        if (r <= 0)
        {
            last_err = r;
            close_log(r == 0 ? "by host" : "short frame");
            return;
        }
        r += 4;
    }
    command_t k = {0};
    if (len != (uint32_t)r || r < 5 || r > (ext ? 320 : 15) || !parse_command(in + 4, in + r, ext, &k))
    {
        close_log("bad frame");
        return;
    }
    int32_t cmd = k.cmd;
    bool have_seq = k.have_seq;
    uint32_t seq = k.seq;
    if (have_seq && seq != expected_cmd)
    {
        log_args_t a = {0};
        log_arg_num(&a, (int32_t)seq);
        log_arg_num(&a, (int32_t)expected_cmd);
        log_fn(LOG_WARN, 37, 6731, 1, &a);
        close_log("command sequence");
        return;
    }
    if (have_seq)
    {
        expected_cmd = seq + 1;
    }
    uint8_t c = (uint8_t)cmd;
    static const uint16_t ack[7] = {6739, 6732, 6733, 6735, 6736, 6737, 6738};
    fn_record(LOG_INFO, 37, c <= 6 ? ack[c] : 6739, 0, NULL, have_seq, seq, false);
    debug_verbose("rtnl: log-host command %ld\n", (long)cmd);
    /* with a sequence number the whole buffer goes first. */
    if (have_seq)
    {
        while (sock >= 0 && !buffer_empty() && !wifi_changed())
        {
            monitor_mark(ACT_LOG);
            if (!send_some())
            {
                close_log("send failed");
            }
        }
    }
    if (c == 2)
    {
        log_args_t a = {0};
        log_arg_num(&a, (int32_t)(100 * mac_last));
        log_fn(LOG_INFO, 37, have_seq ? 6734 : 6743, 0, &a);
        close_log("pause");
        next_attempt += 100 * mac_last; /* the pause, then the normal wait */
    }
    else if (c >= 64 && c <= 73 && ext)
    {
        remote(c, have_seq ? seq : 0, k.arg, k.arg_len);
    }
    else if (c == 6)
    {
        cloud_sync_request(false);
    }
    else if (c == 63)
    {
        ext_hello(); /* the log host speaks the extension protocol */
    }
    else if (c >= 3 && c <= 5)
    {
        debug_verbose("rtnl: power cmd %u ignored\n", c);
    }
}

static void open_log(uint32_t s)
{
    attempts++;
    int16_t sk = cloud_log_connect(s);
    unsigned n = attempts > 4 ? 4 : attempts;
    next_attempt = tick_ms() + 1000 + 1000 * (1u << n);
    if (sk < 0)
    {
        return;
    }
    last_err = 0;
    conn_session = s;
    sock = sk;
    expected_cmd = 0;
    remote_on = false;
    sock_timeout(sock, 10);
    debug_printf("rtnl: log open, %lu attempts\n", (unsigned long)attempts);
    identity_to_first();
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t d = discards;
    discards = 0;
    xSemaphoreGive(lock);
    log_args_t a = {0};
    log_arg_num(&a, sock);
    log_arg_num(&a, (int32_t)attempts);
    log_arg_num(&a, (int32_t)d);
    log_fn(LOG_INFO, 37, 7548, 0, &a);
    identity();
}

static void task(void *arg)
{
    (void)arg;
    uint32_t last_check = 0, last_recv = 0;
    for (;;)
    {
        vTaskDelay(50);
        monitor_mark(ACT_LOG);
        uint32_t now = tick_ms();
        uint32_t s = net_session();
        if (sock >= 0 && wifi_changed())
        {
            close_log("Wi-Fi change");
        }
        if (cloud_log_yield_wanted())
        {
            /* The socket goes to a request to the API host
             * (cloud.c). */
            close_log("TLS socket for a request");
            continue;
        }
        if (sock < 0)
        {
            if (now - last_check >= 1000)
            {
                last_check = now;
                xSemaphoreTake(lock, portMAX_DELAY);
                if (used * 100 / BUF_SIZE > 80)
                {
                    while (used && used * 100 / BUF_SIZE > 50)
                    {
                        remove_front(frame_len(0));
                    }
                }
                xSemaphoreGive(lock);
            }
            if (s && round_session == s && net_state() >= 4 && (int32_t)(now - next_attempt) >= 0)
            {
                open_log(s);
            }
            continue;
        }
        while (sock >= 0 && !buffer_empty() && !wifi_changed())
        {
            monitor_mark(ACT_LOG);
            if (!send_some())
            {
                close_log("send failed");
                break;
            }
        }
        if (sock >= 0 && !wifi_changed() && now - last_recv >= 1000)
        {
            last_recv = now;
            commands();
        }
        if (sock >= 0 && !wifi_changed())
        {
            status_on_change();
        }
    }
}

void log_init(void)
{
    lock = xSemaphoreCreateMutexStatic(&lock_mem);
    identity(); /* the start-up instance */
}

void log_start(uint8_t mac_last_byte)
{
    /* Worst case 2416 B (make stackcheck): remote command 64 storing a file
     * (command frame of 320 B, ext.c, FatFs, SD), +25 %. */
    static StackType_t stack[TASK_STACK(848)];
    static StaticTask_t tcb;
    mac_last = mac_last_byte;
    log_task = xTaskCreateStatic(task, "rtnl", TASK_STACK(848), NULL, 2, stack, &tcb);
}
