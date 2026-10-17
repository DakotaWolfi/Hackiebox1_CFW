/*
 * Requests to the API host: server configuration, TLS with the three
 * certificate files, request format, answers, name lookup and connect, time,
 * update check
 * (installing an item follows).
 * On the ESP32 box only the requests of its original firmware, the update
 * check after the hello.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "ff.h"
#include "port.h"
#include "buildblock.h"
#include "clock.h"
#include "crc32.h"
#include "cloud.h"
#include "debug.h"
#include "der.h"
#include "ext.h"
#include "flashfs.h"
#include "handover.h"
#include "lowram.h"
#include "monitor.h"
#include "net.h"
#include "page.h"
#include "sdfiles.h"
#include "settings.h"
#include "sound.h"
#include "player.h"
#include "rtnl.h"
#include "ledui.h"
#include "taf.h"
#include "tick.h"
#include "update.h"
#include "sha256.h"
#include "sock_port.h"
#include "bootstate.h"
#include "version.h"

/* 4096 bytes of work space for the activity (headers, scans, room). Word
 * aligned for the card's DMA: the ESP32's SD driver writes a buffer at an
 * odd address one sector at a time through a bounce buffer, which took the
 * card 12 ms per 4 KB of a download and made playback stutter (device run
 * 2026-10-14). */
static uint8_t work[TAF_BLOCK] __attribute__((aligned(4)));

/* Figure events wait while an online round or a
 * synchronisation runs. */
static volatile bool round_running, sync_running;

bool cloud_sync_running(void)
{
    return sync_running;
}

bool cloud_holds_figures(void)
{
    return round_running || sync_running;
}

/* --- server configuration --- */

typedef struct
{
    bool valid;
    bool is_ip;
    uint32_t ip;
    char host[66];
    uint16_t port;
    char ca[24]; /* CA file on the flash, "/cert/ca.der" by default */
#ifdef CFW_ESP32
    char ntp[66]; /* time server, "pool.ntp.org" by default */
#endif
} server_cfg_t;
static server_cfg_t cfg;

static bool parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++)
    {
        if (!isdigit((unsigned char)*s))
        {
            return false;
        }
        unsigned n = 0, digits = 0;
        while (isdigit((unsigned char)*s))
        {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || n > 255)
            {
                return false;
            }
        }
        v = v << 8 | n;
        if (part < 3 && *s++ != '.')
        {
            return false;
        }
    }
    if (*s)
    {
        return false;
    }
    *ip = v;
    return true;
}

static bool vendor(const char *h)
{
    static const char *const domains[] = {"tbs.toys", "bxcl.de", "boxine.de", "tonies.com", "tonies.de",
                                          "tonies.cloud"};
    size_t n = strlen(h);
    for (unsigned i = 0; i < sizeof domains / sizeof domains[0]; i++)
    {
        size_t d = strlen(domains[i]);
        if (n == d && !strcasecmp(h, domains[i]))
        {
            return true;
        }
        if (n > d && h[n - d - 1] == '.' && !strcasecmp(h + n - d, domains[i]))
        {
            return true;
        }
    }
    return false;
}

static bool valid_name(char *h)
{
    size_t n = strlen(h);
    if (n && h[n - 1] == '.')
    {
        h[--n] = 0;
    }
    if (n < 1 || n > 64 || h[0] == '.' || h[0] == '-' || h[n - 1] == '.' || h[n - 1] == '-')
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = h[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.'))
        {
            return false;
        }
        if (c == '.' && h[i + 1] == '.')
        {
            return false;
        }
    }
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
    {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
    {
        *--e = 0;
    }
    return s;
}

/* the text of /cfw/config.txt (terminated,
 * changed in place) into c; NULL when it names a valid server, else the
 * reason: "none" (no host line), "host", "vendor", "port". */
static const char *config_parse(char *text, server_cfg_t *c)
{
    char host[66] = "", port[16] = "", ca[16] = "";
    bool have_host = false, long_host = false, have_port = false;
    memset(c, 0, sizeof *c);
    strcpy(c->ca, "/cert/ca.der");
#ifdef CFW_ESP32
    strcpy(c->ntp, "pool.ntp.org");
#endif
    for (char *line = text; line && *line;)
    {
        char *next = strchr(line, '\n');
        if (next)
        {
            *next++ = 0;
        }
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r')
        {
            line[l - 1] = 0;
        }
        char *eq = strchr(line, '=');
        char *t = trim(line);
        if (*t && *t != '#' && eq)
        {
            *eq = 0;
            char *key = trim(t), *val = trim(eq + 1);
            if (!strcasecmp(key, "host"))
            {
                snprintf(host, sizeof host, "%s", val);
                have_host = true;
                long_host = strlen(val) >= sizeof host;
            }
            else if (!strcasecmp(key, "ca"))
            {
                snprintf(ca, sizeof ca, "%s", val);
            }
            else if (!strcasecmp(key, "port"))
            {
                snprintf(port, sizeof port, "%s", val);
                have_port = true;
            }
#ifdef CFW_ESP32
            else if (!strcasecmp(key, "ntp"))
            {
                /* a name or an IP address, never a vendor host */
                uint32_t ip;
                char name[66];
                snprintf(name, sizeof name, "%s", val);
                if (strlen(val) < sizeof name && (parse_ip(name, &ip) || (valid_name(name) && !vendor(name))))
                {
                    strcpy(c->ntp, name);
                }
                else
                {
                    debug_printf("cloud: ntp \"%s\" invalid, %s used\n", val, c->ntp);
                }
            }
#endif
        }
        line = next;
    }
    /* ca: the name of the CA file in /cert/, for boxes whose CA lies in
     * another file (for example c2.der after the alternative-CA patch). */
    if (*ca)
    {
        bool ok = strlen(ca) <= 12;
        for (char *k = ca; *k && ok; k++)
        {
            ok = isalnum((unsigned char)*k) || *k == '.' || *k == '_' || *k == '-';
        }
        if (ok)
        {
            snprintf(c->ca, sizeof c->ca, "/cert/%s", ca);
        }
        else
        {
            debug_printf("cloud: ca \"%s\" invalid, /cert/ca.der used\n", ca);
        }
    }
    if (!have_host)
    {
        return "none";
    }
    unsigned long p = 443;
    if (have_port)
    {
        char *end;
        p = strtoul(port, &end, 10);
        if (!*port || *end || p < 1 || p > 65535)
        {
            return "port";
        }
    }
    if (parse_ip(host, &c->ip))
    {
        c->is_ip = true;
    }
    else if (long_host || !valid_name(host))
    {
        return "host";
    }
    else if (vendor(host))
    {
        return "vendor";
    }
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = (uint16_t)p;
    c->valid = true;
    return NULL;
}

static void config_read(void)
{
    char *text = (char *)work; /* the work buffer is free at start */
    FIL f;
    UINT n = 0;
    const char *why = "missing";
    if (f_open(&f, "/cfw/config.txt", FA_READ) == FR_OK)
    {
        why = f_size(&f) > 1024 ? "longer than 1024 B" : NULL;
        if (!why)
        {
            f_read(&f, text, 1024, &n);
        }
        f_close(&f);
    }
    text[n] = 0;
    const char *r = config_parse(text, &cfg); /* without a file: the default CA */
    if (why)
    {
        cfg.valid = false;
    }
    else
    {
        why = r;
    }
    if (why)
    {
        debug_printf("cloud: /cfw/config.txt: %s, no cloud\n", why);
    }
    else
    {
        debug_printf("cloud: server %s port %u, CA %s\n", cfg.host, cfg.port, cfg.ca);
    }
}

/* --- connection and HTTP --- */

const char *cloud_error_name(cloud_error_t e)
{
    static const char *const names[] = {"ok", "name resolution", "connection not established",
                                        "service unavailable", "content refused", "range not satisfiable",
                                        "unexpected status", "protocol error", "timeout", "transfer error",
                                        "Wi-Fi change or cancelled"};
    return e <= CE_CANCELLED ? names[e] : "?";
}

/* The socket and the connection checks; small enough for the stack of
 * the log activity (cloud_log_connect). */
typedef struct
{
    int16_t sock;
    uint32_t session;
    uint32_t start, deadline_ms; /* 0: no deadline */
    volatile bool *cancel;       /* set before the connect */
    bool one_lookup;             /* the log host: a single lookup */
    uint8_t act;                 /* activity marked at each check */
    long err;                    /* driver code of a failed lookup or connect */
} link_t;

typedef struct
{
    link_t k;
    uint8_t head[512];
    unsigned have, used; /* bytes in head, of them consumed */
    int status;
    uint32_t length; /* announced body length */
    bool length_bad;
    bool has_range;
    uint32_t range_first;
    uint32_t body_left;
} conn_t;

/* Called between network operations, at least once per second (receive
 * timeout 1 s): also the progress mark of the activity. */
static cloud_error_t checks(const link_t *c)
{
    monitor_mark((activity_t)c->act);
    if (net_session() != c->session || (c->cancel && *c->cancel))
    {
        return CE_CANCELLED;
    }
    if (c->deadline_ms && tick_ms() - c->start >= c->deadline_ms)
    {
        return CE_TIMEOUT;
    }
    return CE_OK;
}

/* A valid server configuration is in effect. */
bool cloud_configured(void)
{
    return cfg.valid;
}

static cloud_error_t resolve(link_t *c, uint32_t *ip)
{
    c->err = -32767;
    if (!cfg.valid)
    {
        return CE_NAME;
    }
    if (cfg.is_ip)
    {
        *ip = cfg.ip;
        return CE_OK;
    }
    for (int attempt = 0; attempt < (c->one_lookup ? 1 : 3); attempt++)
    {
        cloud_error_t e = checks(c);
        if (e)
        {
            return e;
        }
        long r = sock_resolve(cfg.host, ip);
        if (r >= 0)
        {
            return CE_OK;
        }
        debug_printf("cloud: lookup %s %ld\n", cfg.host, r);
        c->err = r;
    }
    return CE_NAME;
}

static void close_link(link_t *c)
{
    if (c->sock >= 0)
    {
        sock_close(c->sock);
    }
    c->sock = -1;
}

static void close_conn(conn_t *c)
{
    close_link(&c->k);
}

/* a socket operation that fails for lack of memory or buffers
 * in the network processor asks for a network restart. */
static volatile bool restart_wanted;
static volatile unsigned nobufs_count;

static long nobufs(long r)
{
    if (r == SOCK_NOBUFS)
    {
        nobufs_count++;
        if (!restart_wanted)
        {
            debug_printf("cloud: ENOBUFS -> restart\n");
        }
        restart_wanted = true;
    }
    return r;
}

bool cloud_restart_wanted(void)
{
    CRITICAL_ENTER();
    bool w = restart_wanted;
    restart_wanted = false;
    CRITICAL_EXIT();
    return w;
}

/* One socket with the three certificate files and its connect; the
 * driver's result (negative on failure, the socket stays in c->sock). */
static long connect_once(link_t *c, uint32_t ip)
{
    return nobufs(sock_tls_connect(&c->sock, ip, cfg.port, cfg.ca));
}

/* Instead of the usual log reconnect: when all TLS
 * sockets are taken (the log connection, and a download just cancelled,
 * which the network processor keeps for about 25 s until the server stops
 * sending), the log connection gives up its socket for the request and
 * does not reconnect until the request has its connection; a log connect
 * running at that moment is abandoned (its cancel flag). */
static volatile bool log_yields;

bool cloud_log_yield_wanted(void)
{
    return log_yields;
}

static cloud_error_t open_conn(link_t *c)
{
    uint32_t ip;
    cloud_error_t e = resolve(c, &ip);
    if (e)
    {
        return e;
    }
    /* A busy socket, and a network processor that has not yet released
     * the TLS session of the previous request (too many SSL sockets open),
     * are retried with waits of 100, 200, 400 ... ms within 25500 ms of the
     * first attempt. */
    uint32_t t0 = tick_ms();
    bool yielding = false;
    for (unsigned wait = 100;; wait *= 2)
    {
        e = checks(c);
        if (e)
        {
            break;
        }
        long r = connect_once(c, ip);
        if (r >= 0)
        {
            debug_verbose("cloud: TLS connection in %lu ms\n", (unsigned long)(tick_ms() - t0));
            break;
        }
        close_link(c);
        if (sock_tls_full(r) && c->act != ACT_LOG && !yielding)
        {
            debug_printf("cloud: TLS sockets taken, log connection yields\n");
            yielding = log_yields = true;
        }
        if (!sock_busy(r) || tick_ms() - t0 + wait > 25500)
        {
            /* -453 CA does not verify the server, -454 CA file missing,
             * -460 all TLS sockets taken, -461 certificate date (NWP
             * clock), -111 refused */
            debug_printf("cloud: connect%s %lu.%lu.%lu.%lu:%u %ld\n", c->act == ACT_LOG ? " (log)" : "",
                         (unsigned long)(ip >> 24), (unsigned long)(ip >> 16 & 255), (unsigned long)(ip >> 8 & 255),
                         (unsigned long)(ip & 255), cfg.port, r);
            c->err = r;
            e = CE_CONNECT;
            break;
        }
        debug_verbose("cloud: connect busy (%ld), again in %u ms\n", r, wait);
        for (unsigned w = 0; w < wait; w += 500)
        {
            monitor_mark((activity_t)c->act);
            vTaskDelay(wait - w < 500 ? wait - w : 500);
        }
    }
    if (yielding)
    {
        log_yields = false;
    }
    return e;
}

static cloud_error_t send_all(conn_t *c, const void *data, unsigned n)
{
    const uint8_t *p = data;
    while (n)
    {
        cloud_error_t e = checks(&c->k);
        if (e)
        {
            return e;
        }
        long r = nobufs(sock_send(c->k.sock, p, n > 1400 ? 1400 : n));
        if (r == SOCK_AGAIN)
        {
            continue;
        }
        if (r <= 0)
        {
            return CE_TRANSFER;
        }
        p += r;
        n -= (unsigned)r;
    }
    return CE_OK;
}

/* Reads into buf up to max bytes; 0 on a receive timeout (the caller
 * checks), negative on an error or close. Up to 4096 bytes per call (the
 * driver takes up to 16000): a content download reads into work, and
 * fewer calls leave more of the time to the network (device run
 * 2026-10-14: 768 calls of 1400 bytes per MB, 7 s of 10 s). */
static long recv_some(conn_t *c, void *buf, unsigned max)
{
    long r = nobufs(sock_recv(c->k.sock, buf, max > sizeof work ? sizeof work : max));
    if (r == SOCK_AGAIN)
    {
        return 0;
    }
    if (r <= 0)
    {
        return -1;
    }
    return r;
}

/* + signs, digits; a minus sign or 2^31 or more is malformed. */
static uint32_t parse_length(const char *s, bool *bad)
{
    bool minus = false;
    while (*s == '+' || *s == '-')
    {
        minus |= *s++ == '-';
    }
    uint64_t v = 0;
    while (isdigit((unsigned char)*s))
    {
        v = v * 10 + (uint64_t)(*s++ - '0');
        if (v >= 0x80000000u)
        {
            *bad = true;
        }
    }
    if (minus && v)
    {
        *bad = true;
    }
    if (minus)
    {
        *bad = true;
    }
    return (uint32_t)v;
}

static const char *header_line(const char *head, const char *name)
{
    const char *p = strstr(head, name);
    while (p && (p < head + 2 || p[-1] != '\n' || p[-2] != '\r'))
    {
        p = strstr(p + 1, name);
    }
    return p ? p + strlen(name) : NULL;
}

/* The CR LF CR LF ending the header block in the n bytes read, or NULL. */
static char *head_end(uint8_t *h, unsigned n)
{
    for (unsigned i = 0; i + 4 <= n; i++)
    {
        if (!memcmp(h + i, "\r\n\r\n", 4))
        {
            return (char *)h + i;
        }
    }
    return NULL;
}

static cloud_error_t read_head(conn_t *c)
{
    c->have = c->used = 0;
    char *end = NULL;
    while (!end)
    {
        cloud_error_t e = checks(&c->k);
        if (e)
        {
            return e;
        }
        if (c->have >= sizeof c->head - 1)
        {
            return CE_PROTOCOL;
        }
        long r = recv_some(c, c->head + c->have, (unsigned)(sizeof c->head - 1 - c->have));
        if (r < 0)
        {
            return CE_TRANSFER;
        }
        c->have += (unsigned)r;
        c->head[c->have] = 0;
        /* The body may follow in the same read and hold any bytes, zero
         * included: the end of the header block is searched in the bytes
         * read, and only the header block must be free of zero bytes. */
        end = head_end(c->head, c->have);
        if (memchr(c->head, 0, end ? (size_t)(end - (char *)c->head) : c->have))
        {
            return CE_PROTOCOL;
        }
    }
    unsigned hlen = (unsigned)(end - (char *)c->head) + 4;
    if (hlen > 508 || memcmp(c->head, "HTTP/1.", 7))
    {
        return CE_PROTOCOL;
    }
    char saved = c->head[hlen];
    c->head[hlen] = 0;
    const char *h = (const char *)c->head;
    bool bad = false;
    int sign = 1;
    const char *s = h + 9;
    while (*s == '+' || *s == '-')
    {
        if (*s++ == '-')
        {
            sign = -sign;
        }
    }
    long st = 0;
    for (; isdigit((unsigned char)*s); s++)
    {
        if (st < 100000) /* clamped: no overflow, still not a known status */
        {
            st = st * 10 + (*s - '0');
        }
    }
    c->status = (int)(sign * st);
    const char *cl = header_line(h, "Content-Length: ");
    c->length = cl ? parse_length(cl, &bad) : 0;
    c->length_bad = bad;
    const char *cr = header_line(h, "Content-Range: bytes ");
    c->has_range = cr != NULL;
    c->range_first = 0;
    if (cr)
    {
        char *e;
        unsigned long first = strtoul(cr, &e, 10), last, total;
        if (*e != '-')
        {
            goto bad_range;
        }
        last = strtoul(e + 1, &e, 10);
        if (*e != '/')
        {
            goto bad_range;
        }
        total = strtoul(e + 1, &e, 10);
        if (last + 1 != total || first > last || total != first + c->length)
        {
            goto bad_range;
        }
        c->range_first = (uint32_t)first;
    }
    c->head[hlen] = (uint8_t)saved;
    c->used = hlen;
    c->body_left = c->length;
    return CE_OK;
bad_range:
    c->head[hlen] = (uint8_t)saved;
    return CE_PROTOCOL;
}

/* Body bytes: the rest of the first read, then the socket; at most the
 * announced length. Returns the bytes read, 0 at the end. */
static long read_body(conn_t *c, void *buf, unsigned max, cloud_error_t *err)
{
    if (!c->body_left)
    {
        return 0;
    }
    if (max > c->body_left)
    {
        max = c->body_left;
    }
    unsigned n = c->have - c->used;
    if (n)
    {
        if (n > max)
        {
            n = max;
        }
        memcpy(buf, c->head + c->used, n);
        c->used += n;
        c->body_left -= n;
        return (long)n;
    }
    for (;;)
    {
        *err = checks(&c->k);
        if (*err)
        {
            return -1;
        }
        long r = recv_some(c, buf, max);
        if (r < 0)
        {
            *err = CE_TRANSFER;
            return -1;
        }
        if (r)
        {
            c->body_left -= (uint32_t)r;
            return r;
        }
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Reads n bytes at offset (negative: from the end) of a flash file; the
 * file system error, 0 when done. */
static long flash_read(const char *name, long offset, uint8_t *buf, unsigned n)
{
    long size = flashfs_size(name);
    if (size < 0)
    {
        return size;
    }
    if (offset < 0)
    {
        offset += size;
    }
    if (offset < 0 || (unsigned long)offset + n > (unsigned long)size)
    {
        return -1;
    }
    long r = flashfs_read(name, (uint32_t)offset, buf, n);
    return r == (long)n ? 0 : (r < 0 ? r : -1);
}

#ifndef CFW_ESP32
/* The bootloader word: the word
 * at offset 104 of the own build block in SRAM, which the bootloader
 * fills in; read through a volatile pointer so that the zero of the image
 * file is not folded in. */
static uint32_t hw_word(void)
{
    static uint32_t pre_img; /* word of /sys/pre-img.bin, once read */
    static bool pre_img_read;
    const volatile uint8_t *b = (const volatile uint8_t *)buildblock_in_sram() + 104;
    uint32_t w = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    const handover_t *h = handover_get();
    if (w || !h->valid)
    {
        return w;
    }
    if (h->bootloader_word)
    {
        return h->bootloader_word;
    }
    /* The original bootloader under the SD bootloader: its build block, the
     * last 112 bytes of the flash file, when valid; read
     * once the network processor has started (a request runs). */
    if (!pre_img_read)
    {
        uint8_t bb[112];
        pre_img_read = true;
        if (!flash_read("/sys/pre-img.bin", -112, bb, sizeof bb) && le32(bb) == 0xBEAC0005u &&
            le32(bb + 108) == 0xBEAC0005u)
        {
            pre_img = le32(bb + 104);
        }
        debug_verbose("cloud: bootloader word %lu (pre-img)\n", (unsigned long)pre_img);
    }
    return pre_img;
}
#endif

/* the build number of the original firmware 3.4.0, the
 * identity towards the API host when the box's own original firmware has
 * none to read; cloud_build, read at start, is the one used. */
#define CLOUD_BUILD 1779199517u
#ifndef CFW_ESP32
static uint32_t cloud_build = CLOUD_BUILD;
#endif

/* request line, Host, User-Agent and the extra
 * header lines, then the X-CFW-Build header and the body. */
static cloud_error_t request(conn_t *c, const char *method, const char *path, const char *extra,
                             const void *body, unsigned body_len)
{
    static char req[512] LOWRAM; /* written before use: no-init low SRAM */
    char length[32] = "";
    if (body)
    {
        snprintf(length, sizeof length, "Content-Length: %u\r\n", body_len);
    }
#ifdef CFW_ESP32
    /* the User-Agent of the original ESP firmware. */
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n%s%sX-CFW-Build: %lu\r\n\r\n",
                     method, path, cfg.host, update_original_agent(), length, extra ? extra : "",
                     (unsigned long)CFW_BUILD_TIME);
#else
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: TB/%lu SP/%lu HW/%lu\r\n%s%sX-CFW-Build: %lu\r\n\r\n",
                     method, path, cfg.host, (unsigned long)cloud_build, (unsigned long)net_sp_version(),
                     (unsigned long)hw_word(), length, extra ? extra : "", (unsigned long)CFW_BUILD_TIME);
#endif
    if (n <= 0 || n >= (int)sizeof req)
    {
        return CE_PROTOCOL;
    }
    cloud_error_t e = send_all(c, req, (unsigned)n);
    if (!e && body && body_len)
    {
        e = send_all(c, body, body_len);
    }
    if (!e)
    {
        e = read_head(c);
    }
    return e;
}

static void link_init(link_t *k, uint32_t session, uint32_t deadline, volatile bool *cancel)
{
    memset(k, 0, sizeof *k);
    k->sock = -1;
    k->session = session;
    k->start = tick_ms();
    k->deadline_ms = deadline;
    k->cancel = cancel;
    k->act = ACT_CLOUD;
}

/* A whole exchange with a new connection; cancel (may be
 * NULL) is polled from the name lookup on. */
static cloud_error_t exchange(conn_t *c, uint32_t session, uint32_t deadline, volatile bool *cancel,
                              const char *method, const char *path, const char *extra, const void *body,
                              unsigned body_len)
{
    memset(c, 0, sizeof *c);
    link_init(&c->k, session, deadline, cancel);
    /* No request without a network session: checks() would
     * pass with session 0 == net_session() 0. */
    cloud_error_t e = session ? open_conn(&c->k) : CE_CANCELLED;
    if (!e)
    {
        e = request(c, method, path, extra, body, body_len);
    }
    if (!e && c->status == 503)
    {
        e = CE_UNAVAILABLE;
    }
    debug_printf("cloud: %s %s -> %d, length %lu%s (%s)\n", method, path, e ? -1 : c->status,
                 (unsigned long)c->length, c->length_bad ? " malformed" : "", cloud_error_name(e));
    return e;
}

/* --- log-connection outcome --- */

static volatile bool log_open;          /* a log connection is open */
static volatile bool outcome_new;       /* an outcome not yet checked */
static volatile uint16_t outcome_state; /* 998, 976, 992 */
static volatile int32_t outcome_code;

static void log_outcome(uint16_t state, long code, bool open)
{
    CRITICAL_ENTER();
    outcome_state = state;
    outcome_code = (int32_t)code;
    outcome_new = true;
    log_open = open;
    CRITICAL_EXIT();
}

/* The log connection: same host, port and TLS,
 * one name lookup. Only the small link_t lies
 * on the caller's stack. */
int16_t cloud_log_connect(uint32_t session)
{
    link_t k;
    link_init(&k, session, 0, &log_yields);
    k.one_lookup = true;
    k.act = ACT_LOG; /* on the stack of the log activity */
    if (!cfg.valid || !session)
    {
        return -1;
    }
    cloud_error_t e = open_conn(&k);
    /* the outcome of an attempt that got as far as the lookup */
    if (e == CE_NAME)
    {
        log_outcome(998, k.err, false);
    }
    else if (e == CE_CONNECT)
    {
        log_outcome(976, k.err, false);
    }
    else if (!e)
    {
        log_outcome(992, 0, true);
    }
    return e ? -1 : k.sock;
}

void cloud_log_closed(void)
{
    log_open = false;
}

/* --- time --- */

/* the first answered exchange of the start confirms a trial
 * start (the time request; on the ESP32 box the freshness check). */
static void server_answered(void)
{
    static bool reported;
    if (!reported)
    {
        reported = true;
        bootstate_report(BOOT_CHECK_TIME, true);
    }
}

#ifndef CFW_ESP32
/* Cloud error of the last time request, CE_OK on success. */
static cloud_error_t time_error;

static bool time_request(uint32_t s)
{
    conn_t c;
    unsigned nb = nobufs_count;
    cloud_error_t e = exchange(&c, s, 0, NULL, "GET", "/v1/time", NULL, NULL, 0);
    /* A driver short of memory or buffers counts as a failed connection; any
     * other failure, the clock setting included, as a
     * protocol error. */
    time_error = nobufs_count != nb ? CE_CONNECT : e ? e
                                                     : CE_PROTOCOL;
    if (e || c.status != 200 || c.length_bad || c.length < 1 || c.length > 11)
    {
        close_conn(&c);
        return false;
    }
    char body[12] = {0};
    unsigned got = 0;
    while (got < c.length)
    {
        long r = read_body(&c, body + got, c.length - got, &e);
        if (r <= 0)
        {
            break;
        }
        got += (unsigned)r;
    }
    close_conn(&c);
    if (got < c.length)
    {
        debug_printf("cloud: time body incomplete (%s)\n", cloud_error_name(e));
        return false;
    }
    const char *p = body;
    bool minus = false;
    while (*p == '+' || *p == '-')
    {
        minus ^= *p++ == '-';
    }
    uint32_t t = 0;
    while (isdigit((unsigned char)*p))
    {
        t = t * 10 + (uint32_t)(*p++ - '0');
    }
    if (minus)
    {
        t = (uint32_t)-t;
    }
    /* a time not later than the uptime changes neither clock;
     * the time base moves only after the network processor took the date. */
    if (!clock_valid(t))
    {
        debug_printf("cloud: time %lu <= uptime\n", (unsigned long)t);
        return false;
    }
    civil_t cv;
    clock_to_civil(t, &cv);
    long r = sock_set_date(&cv);
    if (r < 0)
    {
        debug_printf("cloud: date set %ld\n", r);
        return false;
    }
    if (!clock_set(t))
    {
        return false; /* the uptime passed t meanwhile */
    }
    server_answered();
    time_error = CE_OK;
    debug_printf("cloud: clock %lu (%04lu-%02lu-%02lu %02lu:%02lu:%02lu)\n", (unsigned long)t,
                 (unsigned long)cv.year, (unsigned long)cv.month, (unsigned long)cv.day, (unsigned long)cv.hour,
                 (unsigned long)cv.minute, (unsigned long)cv.second);
    return true;
}
#endif

/* --- update check --- */

/* Item 7, the setup page on the card: its CRC-32 (chapter
 * TAF's polynomial) shifted right by one, so that it stays below 2^31; 0
 * when the file is missing. */
static FIL page_file; /* item 7: reading the page or writing the new one */

static uint32_t page_version(void)
{
    FIL *f = &page_file;
    uint32_t crc = 0;
    UINT n = 0;
    if (f_open(f, "/cfw/setup.htm", FA_READ) != FR_OK)
    {
        return 0;
    }
    while (f_read(f, work, sizeof work, &n) == FR_OK && n)
    {
        crc = crc32_ogg(crc, work, n);
    }
    f_close(f);
    return crc >> 1;
}

/* Item 2: the build time of the build block of a slot file
 * (176 bytes before its end), 1 without a valid one, 4294967295 when the
 * file is missing. */
static uint32_t slot_build(const char *name)
{
    uint8_t *buf = work;
    long r = flash_read(name, -176, buf, 176);
    if (r == FLASHFS_MISSING)
    {
        return 4294967295u;
    }
    if (r || le32(buf) != 0xBEAC0005u || le32(buf + 108) != 0xBEAC0005u)
    {
        return 1;
    }
    return le32(buf + 8);
}

#ifndef CFW_ESP32
/* the build number of the box's original firmware, from the
 * build block of the slot the boot info names; 3.4.0 when
 * it has none. */
static void cloud_build_read(void)
{
    char name[] = "/sys/mcuimg1.bin";
    if (!flash_read("/sys/mcubootinfo.bin", 0, work, 1) && work[0] <= 2)
    {
        name[11] += (char)work[0];
    }
    uint32_t b = slot_build(name);
    if (b > 1 && b != 4294967295u)
    {
        cloud_build = b;
    }
    debug_printf("cloud: identity %lu\n", (unsigned long)cloud_build);
}
#endif

static uint32_t current_version(unsigned id)
{
    uint8_t *buf = work;
    long r;
    switch (id)
    {
    case 2:
        return slot_build("/sys/mcuimg1.bin");
    case 3:
#ifdef CFW_ESP32
        return CFW_BUILD_TIME; /* the original's value still open */
#else
        return cloud_build;
#endif
    case 4:
        return net_sp_version();
    case 5:
    {
        r = flash_read("/www/index.html", 0, buf, 176);
        if (r == FLASHFS_MISSING)
        {
            return 4294967295u;
        }
        if (r)
        {
            return 1;
        }
        uint32_t v = 0;
        for (int i = 0; i < 10; i++)
        {
            if (!isdigit(buf[23 + i]))
            {
                return 1;
            }
            v = v * 10 + (uint32_t)(buf[23 + i] - '0');
        }
        return v;
    }
    case 6:
    {
        /* The audio id of the first archive entry (the read
         * at start follows with the prompts). */
        return settings_archive_audio_id();
    }
    }
    return 1;
}

static bool update_checked;
static uint8_t dl_buf[1400] LOWRAM; /* bodies: items, content, prompts, freshness */

/* Item 3: the body streamed into
 * the update module; true when the image is installed. */
static bool install_image(conn_t *c)
{
    if (c->length_bad || !c->length || !update_begin(c->length))
    {
        debug_printf("cloud: image of %lu bytes%s refused\n", (unsigned long)c->length,
                     c->length_bad ? " (malformed length)" : "");
        return false;
    }
    ledui_request(LED_FIRMWARE_DOWNLOAD);
    uint32_t got = 0;
    while (got < c->length)
    {
        cloud_error_t e = CE_OK;
        long n = read_body(c, dl_buf, sizeof dl_buf, &e); /* checks() marks the activity */
        if (n <= 0 || !update_write(dl_buf, (size_t)n))
        {
            debug_printf("cloud: image ended at %lu of %lu (%s)\n", (unsigned long)got,
                         (unsigned long)c->length, n <= 0 ? cloud_error_name(e ? e : CE_TRANSFER) : "write");
            update_abort();
            return false;
        }
        got += (uint32_t)n;
        monitor_mark(ACT_CLOUD);
    }
    close_conn(c);
    return update_finish() == UPDATE_INSTALLED;
}

/* the setup page as item 7, written to the card through a
 * temporary file; no restart. */
static bool install_page(conn_t *c)
{
    FIL *f = &page_file;
    uint32_t got = 0;
    UINT w = 0;
    if (c->length_bad || !c->length || c->length > 65536 ||
        f_open(f, "/cfw/setup.tmp", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    bool ok = true;
    while (ok && got < c->length)
    {
        cloud_error_t e = CE_OK;
        long n = read_body(c, dl_buf, sizeof dl_buf, &e);
        ok = n > 0 && f_write(f, dl_buf, (UINT)n, &w) == FR_OK && w == (UINT)n;
        got += ok ? (uint32_t)n : 0;
    }
    ok = f_close(f) == FR_OK && ok;
    if (ok)
    {
        f_unlink("/cfw/setup.htm");
        ok = f_rename("/cfw/setup.tmp", "/cfw/setup.htm") == FR_OK;
    }
    if (!ok)
    {
        f_unlink("/cfw/setup.tmp");
    }
    debug_printf("cloud: setup page %s, %lu bytes\n", ok ? "installed" : "abandoned", (unsigned long)got);
    return ok;
}

/* the items 4, 5, 2, 6, 3 of the original firmware under
 * either bootloader; item 2 is never installed, item 3 only into a free SD
 * slot. An answer 200 that is not installed is abandoned. */
static void update_check(uint32_t s)
{
#ifdef CFW_ESP32
    /* without the items 4, 5 and 6; item 2 still open. */
    static const unsigned ids[1] = {3};
    bool slot = update_free_slot();
#else
    static const unsigned ids[5] = {4, 5, 2, 6, 3};
    bool slot = handover_get()->valid && update_free_slot();
#endif
    update_checked = true;
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
    {
        char path[48];
        snprintf(path, sizeof path, "/v1/ota/%u?cv=%lu", ids[i], (unsigned long)current_version(ids[i]));
        conn_t c;
        uint32_t deadline = ids[i] == 6 ? 240000 : 60000;
        cloud_error_t e = exchange(&c, s, deadline, NULL, "GET", path, NULL, NULL, 0);
        bool offered = !e && c.status == 200;
        if (offered && ids[i] == 3 && slot)
        {
            bool ok = install_image(&c);
            close_conn(&c);
            debug_printf("cloud: image %s\n", ok ? "installed, restart follows" : "abandoned");
            if (!ok)
            {
                ledui_request(LED_READY_IDLE); /* end the download pattern */
            }
            return; /* installed: no further item; else none in this start */
        }
        close_conn(&c);
        /* items 2 and 3 offered here are abandoned (the
         * request line above logs the status). */
        if (e || (offered && ids[i] != 2 && ids[i] != 3))
        {
            return; /* no further item in this start */
        }
    }
}

/* item 7 once in a start with a hello, after the freshness
 * check session that brought it. */
static bool page_checked;

static void page_check(uint32_t s)
{
    if (page_checked || !ext_hello_received())
    {
        return;
    }
    page_checked = true;
    char path[32];
    snprintf(path, sizeof path, "/v1/ota/%u?cv=%lu", 7u, (unsigned long)page_version());
    conn_t c;
    if (!exchange(&c, s, 60000, NULL, "GET", path, NULL, NULL, 0) && c.status == 200)
    {
        install_page(&c);
    }
    close_conn(&c);
}

/* --- jobs of the activity --- */

typedef enum
{
    JOB_ONLINE,
    JOB_CONTENT,
    JOB_CLAIM,
    JOB_UPDATE
} job_kind_t;

typedef struct
{
    uint8_t kind;
    bool has_token;
    uint32_t session;
    uint32_t serial, offset, if_range;
    uint8_t uid[8];
    uint8_t token[32];
} job_t;

static StaticQueue_t queue_mem;
static uint8_t queue_buf[6 * sizeof(job_t)];
static QueueHandle_t queue;

/* --- content download --- */

cloud_download_t cloud_dl;
static volatile bool dl_cancel;

static void rUID(const uint8_t uid[8], char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
    {
        out[2 * i] = hex[uid[i] >> 4];
        out[2 * i + 1] = hex[uid[i] & 15];
    }
    out[16] = 0;
}

static void token_header(char *out, size_t n, const uint8_t *token)
{
    static const char hex[] = "0123456789abcdef";
    char t[65];
    for (int i = 0; i < 32; i++)
    {
        t[2 * i] = hex[token[i] >> 4];
        t[2 * i + 1] = hex[token[i] & 15];
    }
    t[64] = 0;
    snprintf(out, n, "Authorization: BD %s\r\n", t);
}

/* Every write of a job to cloud_dl happens only while the job is still the
 * current request: a newer request (cloud_content) or a cancel has its own
 * state there, which the overtaken job must not touch. */
static void dl_end(uint32_t serial, cloud_error_t e, uint32_t session)
{
    cloud_download_t *d = &cloud_dl;
    CRITICAL_ENTER();
    bool mine = d->serial == serial;
    if (mine)
    {
        d->error = e;
        if (e)
        {
            d->failed = true;
        }
        else
        {
            d->done = true;
        }
        d->active = false;
    }
    CRITICAL_EXIT();
    if (!mine)
    {
        return;
    }
    /* Name lookup failures for a download the user waits for. */
    if (e == CE_NAME)
    {
        net_cloud_state(session, 4);
    }
    else if (e == CE_CONNECT)
    {
        net_cloud_state(session, 5);
    }
    else if (!e)
    {
        net_cloud_state(session, 6);
    }
}

static void dl_progress(uint32_t serial, uint32_t pos, uint32_t readable, bool data)
{
    cloud_download_t *d = &cloud_dl;
    CRITICAL_ENTER();
    if (d->serial == serial)
    {
        d->pos = pos;
        d->readable = readable;
        d->data = data;
    }
    CRITICAL_EXIT();
}

/* True from the preparation of the download file until it is closed: the
 * player waits for false before it moves that file. */
static volatile bool file_open;

bool cloud_download_file_open(void)
{
    return file_open;
}

/* the file at the content path, ready to be written from
 * offset o; NULL-free result in f. */
static bool prepare(FIL *f, const char *path, uint32_t z, uint32_t o, const uint8_t uid[8])
{
    FILINFO fi;
    FRESULT r = f_stat(path, &fi);
    bool exists = r == FR_OK;
    if (exists && ((fi.fattrib & AM_DIR) || fi.fsize != z))
    {
        sd_trash(uid);
        if (o)
        {
            debug_printf("cloud: %s size not %lu, abandoned\n", path, (unsigned long)z);
            return false;
        }
        exists = false;
    }
    if (!exists)
    {
        if (!sd_room_for(z, &dl_cancel, work))
        {
            debug_printf("cloud: no room for %lu B, download abandoned\n", (unsigned long)z);
            sd_trash(uid);
            return false;
        }
        if (!sd_make_dirs(path) || f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        {
            debug_printf("cloud: %s not created, abandoned\n", path);
            sd_trash(uid);
            return false;
        }
    }
    else if (f_open(f, path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
    {
        debug_printf("cloud: %s not writable, abandoned\n", path);
        sd_trash(uid);
        return false;
    }
    /* Extended to Z at once, the entry flushed, then written from o on. On
     * a full card f_lseek stops at the last free cluster; that
     * file, and one whose chain is broken, goes to the trash. */
    if (f_lseek(f, z) != FR_OK || f_tell(f) != z || f_sync(f) != FR_OK || f_lseek(f, o) != FR_OK)
    {
        debug_printf("cloud: %s not extendable to %lu B, abandoned\n", path, (unsigned long)z);
        f_close(f);
        sd_trash(uid);
        return false;
    }
    /* As the original box: the hidden attribute
     * (outdated mark, "live" bit) is set while the download runs and
     * cleared only at its completion, so a file whose download broke off
     * never counts as complete, also when Z is a multiple of 4096. f_sync
     * keeps the attribute (it only adds the archive bit). */
    sd_set_outdated(path, true);
    return true;
}

static void content_download(const job_t *j)
{
    cloud_download_t *d = &cloud_dl;
    const uint32_t serial = j->serial;
    if (serial != d->serial)
    {
        return; /* overtaken by a newer request */
    }
    uint32_t session = net_session();
    if (!cfg.valid || !session)
    {
        /* Without a server configuration as a failed name lookup; without a
         * network session the request is not made and the connection state
         * selects the indication. */
        debug_printf("cloud: content request without %s\n", cfg.valid ? "network" : "server configuration");
        dl_end(serial, cfg.valid ? CE_CANCELLED : CE_NAME, session);
        return;
    }
    char ruid[17], path_url[48], extra[160] = "", line[100];
    rUID(j->uid, ruid);
    snprintf(path_url, sizeof path_url, "/v%c/content/%s", j->has_token ? '2' : '1', ruid);
    if (j->offset)
    {
        snprintf(line, sizeof line, "Range: bytes=%lu-\r\nIf-Range: %lu\r\n", (unsigned long)j->offset,
                 (unsigned long)j->if_range);
        strcat(extra, line);
    }
    if (j->has_token)
    {
        token_header(line, sizeof line, j->token);
        strcat(extra, line);
    }
    conn_t c;
    uint32_t t_req = tick_ms();
    cloud_error_t e = exchange(&c, session, 0, &dl_cancel, "GET", path_url, extra, NULL, 0);
    uint32_t t_hdr = tick_ms();
    CRITICAL_ENTER();
    if (d->serial == serial)
    {
        d->status = e ? -1 : c.status;
    }
    CRITICAL_EXIT();
    if (!e)
    {
        bool ranged = j->offset != 0;
        if (c.status == 200 || (c.status == 206 && ranged))
        {
            if (c.status == 206 && (!c.has_range || c.range_first != j->offset))
            {
                e = CE_PROTOCOL;
            }
            else if (c.length_bad || c.length < 1024)
            {
                e = CE_PROTOCOL;
            }
        }
        else if (c.status == 503)
        {
            e = CE_UNAVAILABLE;
        }
        else if (c.status == 416 && ranged)
        {
            e = CE_RANGE; /* the trash and the new request are the player's */
        }
        else if (c.status >= 400 && c.status < 500)
        {
            e = CE_REFUSED;
        }
        else
        {
            e = CE_STATUS;
        }
    }
    if (!e && (dl_cancel || d->serial != serial))
    {
        e = CE_CANCELLED; /* overtaken while the header arrived: nothing created */
    }
    if (e)
    {
        close_conn(&c);
        debug_printf("cloud: content %s failed (%s)\n", ruid, cloud_error_name(e));
        dl_end(serial, e, session);
        return;
    }
    uint32_t o = c.status == 206 ? j->offset : 0;
    uint32_t z = o + c.length;
    char path[32];
    sd_content_path(j->uid, path);
    static FIL f;
    file_open = true;
    if (!prepare(&f, path, z, o, j->uid))
    {
        file_open = false;
        close_conn(&c);
        dl_end(serial, CE_CANCELLED, session);
        return;
    }
    CRITICAL_ENTER();
    if (d->serial == serial)
    {
        d->size = z;
    }
    CRITICAL_EXIT();
    dl_progress(serial, o, o & ~511u, false);
    uint32_t t0 = tick_ms();
    /* time to first sound: connection and answer header, then the file */
    debug_verbose("cloud: header after %lu ms, file after %lu ms\n", (unsigned long)(t_hdr - t_req),
                  (unsigned long)(t0 - t_hdr));
    debug_verbose("cloud: downloading %s, %lu bytes from %lu\n", path, (unsigned long)c.length, (unsigned long)o);
    uint32_t pos = o;
    /* The body is collected in work up to the next 4096-byte boundary of
     * the file and written as whole sectors in one transfer: written one
     * read (1400 bytes) at a time, the card took about 12 ms for each of
     * the 1500 transfers per MB (device run 2026-10-14, 30 KB/s). work is
     * free once prepare() is done. Rate diagnostics at the end: time waiting
     * for the network, in f_write and in the Opus decoder. */
    uint32_t r_net = 0, r_wr = 0;
    player_decode_ms = 0;
    unsigned fill = 0; /* bytes in work from pos - fill on, not yet written */
    while (pos < z)
    {
        uint32_t end = ((pos - fill) | (sizeof work - 1)) + 1;
        if (end > z)
        {
            end = z;
        }
        uint32_t ta = tick_ms();
        long n = read_body(&c, work + fill, (unsigned)(end - pos), &e);
        uint32_t tb = tick_ms();
        r_net += tb - ta;
        if (n <= 0)
        {
            if (!e)
            {
                e = CE_TRANSFER;
            }
            break;
        }
        pos += (uint32_t)n;
        fill += (unsigned)n;
        if (pos < end)
        {
            continue;
        }
        UINT w;
        if (f_write(&f, work, fill, &w) != FR_OK || w != fill)
        {
            debug_printf("cloud: write to %s failed at %lu\n", path, (unsigned long)(pos - fill));
            e = CE_CANCELLED;
            pos -= fill - (unsigned)w;
            fill = 0;
            break;
        }
        fill = 0;
        r_wr += tick_ms() - tb;
        /* FatFs writes a sector of the file buffer back only when the
         * next sector is begun (ff.c, f_write): the sector holding the
         * last byte written may still be in the buffer. */
        dl_progress(serial, pos, (pos - 1) & ~511u, true);
    }
    /* After a broken transfer the bytes already received are kept, so a
     * resumed download starts behind them. */
    if (fill)
    {
        UINT w = 0;
        (void)f_write(&f, work, fill, &w);
        pos -= fill - (unsigned)w;
    }
    close_conn(&c);
    f_close(&f);
    debug_printf("cloud: rate net %lu wr %lu dec %lu ms\n", (unsigned long)r_net, (unsigned long)r_wr,
                 (unsigned long)player_decode_ms);
    if (!e && pos == z)
    {
        sd_set_outdated(path, false); /* complete: the mark set by prepare() goes */
    }
    debug_printf("cloud: download of %s %s at %lu of %lu after %lu ms\n", path, e ? "ended" : "complete",
                 (unsigned long)pos, (unsigned long)z, (unsigned long)(tick_ms() - t0));
    if (e && pos <= 8192)
    {
        sd_trash(j->uid);
    }
    file_open = false;
    dl_progress(serial, pos, pos, pos > o);
    dl_end(serial, e, session);
}

void cloud_content(const uint8_t uid[8], const uint8_t *token, uint32_t offset, uint32_t if_range)
{
    job_t j = {.kind = JOB_CONTENT, .offset = offset, .if_range = if_range};
    memcpy(j.uid, uid, 8);
    if (token)
    {
        j.has_token = true;
        memcpy(j.token, token, 32);
    }
    cloud_download_t *d = &cloud_dl;
    CRITICAL_ENTER();
    /* A request for the rest of the same download from its position keeps what
     * the card holds readable: the
     * playback goes on up to there; any other request starts at 0. */
    bool rest = offset && offset == d->pos && !memcmp(d->uid, uid, 8);
    /* Serial first, then the flag: the activity clears the flag before
     * it compares the serial of the next job, so a job is either skipped
     * or sees the flag (the running one stops at its next check). */
    j.serial = ++d->serial;
    dl_cancel = true;
    d->active = true;
    d->done = d->failed = d->data = false;
    d->error = CE_OK;
    d->status = 0;
    d->pos = rest ? offset : 0;
    d->readable = rest ? offset & ~511u : 0;
    d->size = rest ? d->size : 0;
    memcpy(d->uid, uid, 8);
    CRITICAL_EXIT();
    if (!queue || xQueueSend(queue, &j, 0) != pdTRUE)
    {
        CRITICAL_ENTER();
        if (d->serial == j.serial)
        {
            d->failed = true;
            d->error = CE_CANCELLED;
            d->active = false;
        }
        CRITICAL_EXIT();
    }
}

bool cloud_content_running(const uint8_t uid[8])
{
    CRITICAL_ENTER();
    bool r = cloud_dl.active && !cloud_dl.failed && !memcmp(cloud_dl.uid, uid, 8);
    CRITICAL_EXIT();
    return r;
}

void cloud_content_cancel(void)
{
    CRITICAL_ENTER();
    cloud_dl.serial++;
    dl_cancel = true;
    cloud_dl.active = false;
    CRITICAL_EXIT();
}

/* --- claim --- */

/* A job of the activity is being carried out (any request to the API
 * host). */
static volatile bool job_running;

void cloud_claim(const uint8_t uid[8], const uint8_t *token)
{
#ifdef CFW_ESP32
    /* the original ESP firmware sends no claim. */
    (void)uid;
    (void)token;
    return;
#endif
    job_t j = {.kind = JOB_CLAIM};
    memcpy(j.uid, uid, 8);
    if (token)
    {
        j.has_token = true;
        memcpy(j.token, token, 32);
    }
    if (!queue || uxQueueMessagesWaiting(queue) || cloud_dl.active || job_running || round_running ||
        sync_running)
    {
        debug_printf("cloud: claim not sent, transfer busy\n");
        return;
    }
    xQueueSend(queue, &j, 0);
}

static void claim(const job_t *j)
{
    uint32_t session = net_session();
    if (!cfg.valid || !session)
    {
        return;
    }
    char ruid[17], url[40], extra[100] = "";
    rUID(j->uid, ruid);
    snprintf(url, sizeof url, "/v1/claim/%s", ruid);
    if (j->has_token)
    {
        token_header(extra, sizeof extra, j->token);
    }
    conn_t c;
    cloud_error_t e = exchange(&c, session, 0, NULL, "GET", url, extra, NULL, 0);
    if (!e && c.status == 204 && !c.length_bad && c.length < 512)
    {
        long n;
        while ((n = read_body(&c, dl_buf, sizeof dl_buf, &e)) > 0)
            ;
    }
    else if (!e && c.status == 204)
    {
        debug_printf("cloud: claim answer body %lu B: failed\n", (unsigned long)c.length);
    }
    close_conn(&c);
}

/* --- freshness check --- */

static volatile bool sync_pending = true; /* automatic sync at start-up */
static volatile bool sync_abort;          /* a placed figure ends the session */
static uint32_t prompts_noted;

static bool hex8(const char *n)
{
    if (strlen(n) != 8)
    {
        return false;
    }
    for (int i = 0; i < 8; i++)
    {
        if (!isxdigit((unsigned char)n[i]))
        {
            return false;
        }
    }
    return true;
}

static void hex_bytes(const char *n, uint8_t *out)
{
    for (int i = 0; i < 4; i++)
    {
        char b[3] = {n[2 * i], n[2 * i + 1], 0};
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
}

static unsigned put_entry(uint8_t *p, const uint8_t uid[8], uint32_t audio_id)
{
    p[0] = 0x0A;
    p[1] = 14;
    p[2] = 0x09;
    memcpy(p + 3, uid, 8);
    p[11] = 0x15;
    for (int i = 0; i < 4; i++)
    {
        p[12 + i] = (uint8_t)(audio_id >> (8 * i));
    }
    return 16;
}

/* the content files after the first skip ones, up
 * to max entries; returns the number written, *seen the files passed. */
static unsigned scan(unsigned skip, unsigned max, uint8_t *body, unsigned *seen)
{
    DIR top, sub;
    static FILINFO a, b;
    unsigned n = 0, index = 0;
    *seen = 0;
    if (f_opendir(&top, "CONTENT") != FR_OK)
    {
        return 0;
    }
    while (n < max && f_readdir(&top, &a) == FR_OK && a.fname[0])
    {
        if (!(a.fattrib & AM_DIR) || !hex8(a.fname) || !strncmp(a.fname, "000000", 6))
        {
            continue; /* prompt directories are skipped entirely */
        }
        char dir[20], path[40];
        snprintf(dir, sizeof dir, "CONTENT/%.8s", a.fname);
        if (f_opendir(&sub, dir) != FR_OK)
        {
            continue;
        }
        bool kept = false;
        while (n < max && f_readdir(&sub, &b) == FR_OK && b.fname[0])
        {
            if (b.fattrib & AM_DIR)
            {
                continue;
            }
            snprintf(path, sizeof path, "%.16s/%.12s", dir, b.fname);
            if (!hex8(b.fname))
            {
                f_unlink(path); /* for example a leftover .TMP */
                continue;
            }
            kept = true;
            monitor_mark(ACT_CLOUD); /* a long cache scan */
            if (index++ < skip)
            {
                continue; /* reported by an earlier request */
            }
            ++*seen;
            uint8_t uid[8];
            hex_bytes(a.fname, uid);
            hex_bytes(b.fname, uid + 4);
            static FIL f;
            taf_header_t h;
            taf_state_t st = taf_open(&f, path, &h, work, true);
            if (st == TAF_COMPLETE || st == TAF_INCOMPLETE)
            {
                f_close(&f);
                n++;
                body += put_entry(body, uid, h.audio_id);
            }
            else if (st == TAF_INVALID)
            {
                /* Gone from the directory: the next request must not pass
                 * over one file more. */
                sd_trash(uid);
                index--;
                --*seen;
            }
        }
        f_closedir(&sub);
        if (!kept)
        {
            f_unlink(dir);
        }
    }
    f_closedir(&top);
    return n;
}

static void prompt_uid(unsigned nn, uint8_t uid[8])
{
    memset(uid, 0, 8);
    uid[3] = settings_language();
    uid[7] = (uint8_t)nn;
}

static unsigned prompt_entry(unsigned nn, uint8_t *p)
{
    uint8_t uid[8];
    char path[32];
    prompt_uid(nn, uid);
    sd_content_path(uid, path);
    static FIL f;
    taf_header_t h;
    uint32_t id = 1; /* missing or invalid */
    taf_state_t st = taf_open(&f, path, &h, work, true);
    if (st == TAF_COMPLETE || st == TAF_INCOMPLETE)
    {
        f_close(&f);
        id = h.audio_id;
    }
    return put_entry(p, uid, id);
}

static bool varint_rd(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    for (unsigned sh = 0; sh < 70; sh += 7)
    {
        if (*p >= end)
        {
            return false;
        }
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80))
        {
            *v = r;
            return true;
        }
    }
    return false;
}

typedef struct
{
    uint8_t marks[60][8];
    unsigned n_marks;
    int16_t value[9]; /* fields 2 to 8, INT16_MIN when absent */
} fresh_t;

static bool fresh_decode(const uint8_t *p, unsigned len, fresh_t *r)
{
    const uint8_t *end = p + len;
    r->n_marks = 0;
    for (int i = 0; i < 9; i++)
    {
        r->value[i] = INT16_MIN;
    }
    while (p < end)
    {
        uint64_t key, v;
        if (!varint_rd(&p, end, &key))
        {
            return false;
        }
        unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
        if (field == 1)
        {
            const uint8_t *stop = p + 8;
            if (wire == 2)
            {
                if (!varint_rd(&p, end, &v) || v > (uint64_t)(end - p))
                {
                    return false;
                }
                stop = p + v;
            }
            while (p < stop)
            {
                /* a packed list whose length is not a multiple of 8 does
                 * not decode */
                if (stop - p < 8 || end - p < 8 || r->n_marks >= 60)
                {
                    return false;
                }
                memcpy(r->marks[r->n_marks++], p, 8);
                p += 8;
            }
        }
        else if (field >= 2 && field <= 8)
        {
            if (!varint_rd(&p, end, &v))
            {
                return false;
            }
            if (field == 2 || field == 3 || field == 7)
            {
                if (v > 0xFFFFFFFFu)
                {
                    return false;
                }
                r->value[field] = v > 32767 ? 32767 : (int16_t)v;
            }
            else
            {
                int32_t sv = (int32_t)(uint32_t)v;
                if (sv < -128 || sv > 127)
                {
                    return false;
                }
                r->value[field] = (int16_t)sv;
            }
        }
        else
        {
            switch (wire)
            {
            case 0:
                if (!varint_rd(&p, end, &v))
                {
                    return false;
                }
                break;
            case 1:
                if (end - p < 8)
                {
                    return false;
                }
                p += 8;
                break;
            case 2:
                if (!varint_rd(&p, end, &v) || v > (uint64_t)(end - p))
                {
                    return false;
                }
                p += v;
                break;
            case 5:
                if (end - p < 4)
                {
                    return false;
                }
                p += 4;
                break;
            default:
                return false;
            }
        }
    }
    return true;
}

static void fresh_apply(const fresh_t *r)
{
    uint8_t set = settings_language();
    for (unsigned i = 0; i < r->n_marks; i++)
    {
        const uint8_t *u = r->marks[i];
        char path[32];
        if (u[0] || u[1] || u[2])
        {
            sd_content_path(u, path);
            FILINFO fi;
            if (f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_HID))
            {
                sd_set_outdated(path, true);
                debug_verbose("cloud: %s marked outdated\n", path);
            }
            continue;
        }
        uint32_t nn = (uint32_t)u[4] << 24 | (uint32_t)u[5] << 16 | (uint32_t)u[6] << 8 | u[7];
        if (u[3] != set || nn > 32)
        {
            debug_verbose("cloud: prompt mark set %u number %lu ignored\n", u[3], (unsigned long)nn);
            continue;
        }
        if (nn <= 31)
        {
            prompts_noted |= 1u << nn;
        }
    }
    bool limits = false;
    for (int f = 3; f <= 8; f++)
    {
        int16_t v = r->value[f];
        if (v == INT16_MIN)
        {
            continue;
        }
        uint8_t *dst = NULL;
        int max = 1;
        switch (f)
        {
        case 3:
            dst = &settings.limit_speaker;
            max = 3;
            break;
        case 7:
            dst = &settings.limit_headphones;
            max = 3;
            break;
        case 4:
            dst = &settings.slap_enable;
            break;
        case 5:
            dst = &settings.slap_direction;
            break;
        case 6:
            dst = &settings.reserved6;
            break;
        case 8:
            dst = &settings.led;
            max = 2;
            break;
        }
        if (v < 0 || v > max)
        {
            if (f == 3 || f == 7)
            {
                debug_printf("cloud: volume limit %d ignored\n", v);
            }
            continue;
        }
        if (*dst != v)
        {
            debug_printf("cloud: setting %d changed %u -> %d\n", f, *dst, v);
            *dst = (uint8_t)v;
            limits |= f == 3 || f == 7;
        }
    }
    if (limits)
    {
        sound_limits_changed();
    }
}

/* a prompt into its .TMP file, then into place. */
static cloud_error_t prompt_download(unsigned nn, uint32_t session)
{
    uint8_t uid[8];
    char ruid[17], url[40], path[32], tmp[48];
    prompt_uid(nn, uid);
    rUID(uid, ruid);
    snprintf(url, sizeof url, "/v1/content/%s", ruid);
    sd_content_path(uid, path);
    snprintf(tmp, sizeof tmp, "%s.TMP", path);
    conn_t c;
    cloud_error_t e = exchange(&c, session, 60000, &sync_abort, "GET", url, NULL, NULL, 0);
    if (!e && (c.status != 200 || c.length_bad || c.length < 1024))
    {
        e = c.status == 200 ? CE_PROTOCOL : c.status >= 400 && c.status < 500 ? CE_REFUSED
                                                                              : CE_STATUS;
    }
    if (e)
    {
        close_conn(&c);
        return e;
    }
    static FIL f;
    uint32_t z = c.length, pos = 0;
    /* room for a new .TMP
     * file, extended to Z at once; every failure moves the prompt file
     * (never the .TMP file) to the trash. */
    FILINFO fi;
    bool ok = f_stat(tmp, &fi) == FR_OK || sd_room_for(z, &sync_abort, work);
    bool opened = ok && sd_make_dirs(tmp) && f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    ok = opened && f_lseek(&f, z) == FR_OK && f_tell(&f) == z && f_sync(&f) == FR_OK && f_lseek(&f, 0) == FR_OK;
    if (!ok)
    {
        debug_printf("cloud: %s not prepared for %lu B\n", tmp, (unsigned long)z);
        if (opened)
        {
            f_close(&f);
        }
        close_conn(&c);
        sd_trash(uid);
        return CE_CANCELLED;
    }
    while (pos < z)
    {
        long n = read_body(&c, dl_buf, sizeof dl_buf, &e);
        UINT w;
        if (n <= 0 || f_write(&f, dl_buf, (UINT)n, &w) != FR_OK || w != (UINT)n)
        {
            if (!e)
            {
                e = CE_TRANSFER;
            }
            break;
        }
        pos += (uint32_t)n;
    }
    close_conn(&c);
    f_close(&f);
    if (e)
    {
        if (pos <= 8192)
        {
            sd_trash(uid); /* the prompt file, the .TMP stays */
        }
        return e;
    }
    sd_trash(uid);
    if (f_rename(tmp, path) != FR_OK)
    {
        f_unlink(tmp);
    }
    debug_printf("cloud: prompt %02x downloaded\n", nn);
    return CE_OK;
}

void cloud_sync_abort(void)
{
    sync_abort = true;
}

static volatile bool sync_user;

void cloud_sync_request(bool user)
{
    if (user && cloud_dl.active)
    {
        /* a session started while a download runs fails at
         * once. */
        debug_printf("cloud: sync during a download: failed\n");
        player_sync_result(CE_CANCELLED, true);
        return;
    }
    CRITICAL_ENTER();
    sync_user |= user;
    sync_pending = true;
    CRITICAL_EXIT();
}

/* files directly in the subdirectories of BADBLOCK; nothing is
 * changed. */
static void badblock_stats(void)
{
    DIR top, sub;
    static FILINFO a, b;
    uint32_t n = 0, total = 0, oldest = 0xFFFFFFFFu, newest = 0;
    if (f_opendir(&top, "BADBLOCK") != FR_OK)
    {
        return;
    }
    while (f_readdir(&top, &a) == FR_OK && a.fname[0])
    {
        if (!(a.fattrib & AM_DIR))
        {
            continue;
        }
        char dir[24];
        snprintf(dir, sizeof dir, "BADBLOCK/%.12s", a.fname);
        if (f_opendir(&sub, dir) != FR_OK)
        {
            continue;
        }
        while (f_readdir(&sub, &b) == FR_OK && b.fname[0])
        {
            if (b.fattrib & AM_DIR)
            {
                continue;
            }
            uint32_t t = sd_fat_time(b.fdate, b.ftime);
            n++;
            total += (uint32_t)b.fsize;
            if (t < oldest)
            {
                oldest = t;
            }
            if (t > newest)
            {
                newest = t;
            }
            monitor_mark(ACT_CLOUD);
        }
        f_closedir(&sub);
    }
    f_closedir(&top);
    if (!n)
    {
        return;
    }
    log_args_t l = {0};
    log_arg_num(&l, (int32_t)n);
    log_arg_num(&l, (int32_t)total);
    log_arg_num(&l, (int32_t)oldest);
    log_arg_num(&l, (int32_t)newest);
    log_fn(LOG_INFO, 22, 8608, 0, &l);
}

static cloud_error_t fresh_session(uint32_t s)
{
    unsigned reported = 0, prompt = 0;
    prompts_noted = 0;
    sync_abort = false;
    debug_printf("cloud: freshness check session\n");
    for (;;)
    {
        unsigned seen = 0, n = scan(reported, 60, dl_buf, &seen), len = n * 16;
        reported += seen;
        while (n < 60 && prompt < 25)
        {
            len += prompt_entry(prompt++, dl_buf + len);
            n++;
        }
        if (!n)
        {
            break;
        }
        if (sync_abort)
        {
            return CE_CANCELLED;
        }
        conn_t c;
        cloud_error_t e = exchange(&c, s, 60000, &sync_abort, "POST", "/v1/freshness-check", NULL, dl_buf, len);
        if (!e && (c.status < 200 || c.status > 299 || c.length_bad || c.length > 1024))
        {
            e = c.status >= 200 && c.status <= 299 ? CE_PROTOCOL : CE_STATUS;
        }
        unsigned got = 0;
        while (!e && got < c.length)
        {
            long r = read_body(&c, dl_buf + got, c.length - got, &e);
            if (r <= 0)
            {
                if (!e)
                {
                    e = CE_TRANSFER;
                }
                break;
            }
            got += (unsigned)r;
        }
        close_conn(&c);
        if (e)
        {
            return e;
        }
        static fresh_t r;
        if (!fresh_decode(dl_buf, got, &r))
        {
            return CE_PROTOCOL;
        }
        debug_verbose("cloud: freshness answer: %u marks\n", r.n_marks);
        fresh_apply(&r);
        ext_answer(dl_buf, got); /* fields 100 and 101 */
        if (n < 60 && prompt >= 25)
        {
            break; /* nothing left to report */
        }
    }
    for (unsigned nn = 0; nn <= 24; nn++)
    {
        if (!(prompts_noted & (1u << nn)))
        {
            continue;
        }
        if (sync_abort)
        {
            return CE_CANCELLED;
        }
        cloud_error_t e = prompt_download(nn, s);
        if (e)
        {
            return e;
        }
    }
    badblock_stats(); /* the session ends normally */
    return CE_OK;
}

/* after 1 s without an API request, the latest outcome of a
 * log-connection attempt is posted to /v1/log unless a log connection is
 * open or it equals the pair posted last. */
static void log_post(void)
{
#ifdef CFW_ESP32
    return; /* the original ESP firmware posts no log */
#endif
    static bool posted;
    static uint16_t posted_state;
    static int32_t posted_code;
    uint32_t s = net_session();
    if (!outcome_new || !cfg.valid || !s)
    {
        return;
    }
    CRITICAL_ENTER();
    uint16_t state = outcome_state;
    int32_t code = outcome_code;
    bool open = log_open;
    outcome_new = false;
    CRITICAL_EXIT();
    if (open || (posted && state == posted_state && code == posted_code))
    {
        return; /* dropped */
    }
    char body[24];
    int n = snprintf(body, sizeof body, "%u (%ld)", state, (long)code);
    conn_t c;
    job_running = true;
    cloud_error_t e = exchange(&c, s, 60000, NULL, "POST", "/v1/log", NULL, body, (unsigned)n);
    close_conn(&c); /* the body is not read */
    job_running = false;
    log_args_t a = {0};
    if (e && e != CE_UNAVAILABLE)
    {
        log_arg_num(&a, (int32_t)e);
        log_fn(LOG_WARN, 6, 7006, 6, &a); /* variant f */
        return;
    }
    posted = true;
    posted_state = state;
    posted_code = code;
    if (c.status < 200 || c.status > 299)
    {
        log_arg_text(&a, "POST");
        log_arg_text(&a, "/v1/log");
        log_arg_num(&a, c.status);
        log_fn(LOG_WARN, 6, 116, 2, &a); /* variant b */
    }
}

static cloud_error_t sync_run(uint32_t s, bool user);

static void maybe_sync(void)
{
    uint32_t s = net_session();
    if (!sync_pending || player_busy() || cloud_dl.active || uxQueueMessagesWaiting(queue))
    {
        return;
    }
    if (sync_user && !cfg.valid)
    {
        CRITICAL_ENTER();
        sync_pending = sync_user = false;
        CRITICAL_EXIT();
        player_sync_result(CE_NAME, true);
        return;
    }
    /* An automatic sync waits for state 6, a requested one starts from 4;
     * never without a network session (state 4
     * to 6 can stand for a moment with session 0). */
    if (!cfg.valid || !s || net_state() < (sync_user ? 4 : 6))
    {
        return;
    }
    CRITICAL_ENTER();
    bool user = sync_user;
    sync_pending = sync_user = false;
    CRITICAL_EXIT();
    sync_run(s, user);
}

/* The freshness check session with its indication and result. */
static cloud_error_t sync_run(uint32_t s, bool user)
{
    ledui_request(LED_SYNC);
    sync_running = true;
    monitor_sync_activity(true); /* synchronising is busy */
    cloud_error_t e = fresh_session(s);
    ext_hello_flush();
    page_check(s);
    sync_running = false;
    monitor_sync_activity(false);
    debug_printf("cloud: freshness check session %s\n", e ? cloud_error_name(e) : "complete");
    if (e == CE_NAME)
    {
        net_cloud_state(s, 4);
    }
    else if (e == CE_CONNECT)
    {
        net_cloud_state(s, 5);
    }
    else if (!e)
    {
        net_cloud_state(s, 6);
    }
    player_sync_result((int)e, user);
    return e;
}

/* --- the activity --- */

void cloud_online_round(uint32_t s)
{
    job_t j = {.kind = JOB_ONLINE, .session = s};
    if (!queue)
    {
        debug_printf("cloud: round %lu dropped\n", (unsigned long)s);
        return;
    }
    xQueueSend(queue, &j, 0);
}

static void online_round(uint32_t s)
{
#ifdef CFW_ESP32
    net_time_sync(cfg.ntp); /* also without a server */
#endif
    if (!cfg.valid)
    {
        debug_printf("cloud: no server, round fails\n");
        net_cloud_first_result(s, CE_NAME);
        return; /* no log connection either */
    }
    long r = sock_dns_fallback();
    debug_verbose("cloud: DNS client settings %ld\n", r);
    (void)r;
#ifdef CFW_ESP32
    /* no time request and no update check here; the freshness
     * check session, as the original ESP firmware makes it in every start,
     * is the first exchange. */
    CRITICAL_ENTER();
    bool user = sync_user;
    sync_pending = sync_user = false;
    CRITICAL_EXIT();
    cloud_error_t e = sync_run(s, user);
    net_cloud_first_result(s, e);
    if (!e)
    {
        server_answered();
    }
#else
    bool ok = time_request(s);
    net_cloud_first_result(s, time_error);
    if (ok)
    {
        net_cloud_state(s, 6);
        if (!update_checked)
        {
            monitor_update_round(true); /* limit 40 s */
            update_check(s);
            monitor_update_round(false);
        }
    }
#endif
    log_round_done(s);
}

#ifdef CFW_ESP32
/* the update check and the setup page once in a start, only
 * after the hello of an extension server, when the box is idle. */
static void update_after_hello(void)
{
    uint32_t s = net_session();
    if (update_checked || !ext_hello_received() || !cfg.valid || !s || net_state() < 6 || player_busy() ||
        cloud_dl.active || uxQueueMessagesWaiting(queue))
    {
        return;
    }
    job_running = true;
    monitor_update_round(true); /* limit 40 s */
    update_check(s);
    monitor_update_round(false);
    page_check(s);
    job_running = false;
}
#endif

/* F12 command 73: the update check once more in this start,
 * when the box is idle and no API request is in progress; 0 when
 * scheduled, 1 otherwise. Does not block (log activity). */
int cloud_remote_update_check(void)
{
    uint32_t s = net_session();
    /* state 6: the time of this session applied, as before the check at
     * start */
    if (!queue || !cfg.valid || !s || net_state() < 6 || player_busy() || job_running || sync_running ||
        cloud_dl.active || uxQueueMessagesWaiting(queue))
    {
        return 1;
    }
    job_t j = {.kind = JOB_UPDATE, .session = s};
    return xQueueSend(queue, &j, 0) == pdTRUE ? 0 : 1;
}

/* --- certificate input --- */

/* Compares the flash file with the n bytes in work; 1 equal, 0 different,
 * -1 not readable. */
static int flash_equal(const char *name, unsigned n)
{
    long size = flashfs_size(name);
    if (size < 0)
    {
        return -1;
    }
    if ((unsigned long)size != n)
    {
        return 0;
    }
    int eq = 1;
    for (unsigned off = 0; off < n && eq == 1; off += sizeof dl_buf)
    {
        unsigned k = n - off < sizeof dl_buf ? n - off : sizeof dl_buf;
        if (flashfs_read(name, off, dl_buf, k) != (long)k)
        {
            eq = -1;
        }
        else if (memcmp(dl_buf, work + off, k))
        {
            eq = 0;
        }
    }
    return eq;
}

/* Certificate check: one DER element with tag 0x30 whose encoded length
 * is the size (2 to 4096 bytes), in work. */
static bool cert_der_ok(uint32_t size)
{
    return size <= sizeof work && der_element_size(work, size) == size;
}

/* Check and store certificate i (0 private.der, 1
 * client.der, 2 ca.der: the CA file of the key ca) in work: -1 refused,
 * 0 the flash file equals it (written when it differed, and read back),
 * 1 the write failed. */
int cloud_cert_put(unsigned i, unsigned size)
{
    static const char *const names[3] = {"/cert/private.der", "/cert/client.der", NULL};
    if (i > 2 || !cert_der_ok(size))
    {
        return -1;
    }
    const char *flash = names[i] ? names[i] : cfg.ca;
    int eq = flash_equal(flash, size);
    if (eq != 1)
    {
        long r = flashfs_replace(flash, work, size, true);
        eq = r == (long)size ? flash_equal(flash, size) : -1;
        debug_printf("cloud: %s written: %s\n", flash, eq == 1 ? "ok" : "FAILED");
    }
    return eq != 1;
}

#if PORT_CERT_INPUT
static void cert_input(void)
{
    static const char *const names[3] = {"private.der", "client.der", "ca.der"};
    for (unsigned i = 0; i < 3; i++)
    {
        char card[24];
        snprintf(card, sizeof card, "/cfw/cert/%s", names[i]);
        FIL f;
        UINT n = 0;
        if (f_open(&f, card, FA_READ) != FR_OK)
        {
            continue;
        }
        uint32_t size = (uint32_t)f_size(&f);
        bool ok = size <= sizeof work && f_read(&f, work, size, &n) == FR_OK && n == size;
        f_close(&f);
        int r = ok ? cloud_cert_put(i, size) : -1;
        if (r < 0)
        {
            debug_printf("cloud: %s not one DER element, left on card\n", card);
        }
        if (!r)
        {
            f_unlink(card);
            debug_printf("cloud: %s equals flash, deleted from card\n", card);
        }
    }
}
#endif

/* --- setup page (page.c in this activity) --- */

/* Deletes host and port of /cfw/config.txt and the CA file it names; the
 * vendor CA /cert/ca.der stays, as the original firmware uses it too. The
 * line ca= stays, so that a CA uploaded next goes where the next start
 * looks for it. Takes effect at the next start. */
const char *cloud_server_forget(void)
{
    static FIL f;
    bool vendor = !strcmp(cfg.ca, "/cert/ca.der");
    long d = vendor ? 0 : flashfs_delete(cfg.ca);
    FRESULT r = f_unlink("/cfw/config.txt");
    if (!vendor && (r == FR_OK || r == FR_NO_FILE))
    {
        char line[32];
        UINT w = 0;
        int n = snprintf(line, sizeof line, "ca=%s\n", cfg.ca + 6); /* after "/cert/" */
        r = f_open(&f, "/cfw/config.txt", FA_WRITE | FA_CREATE_ALWAYS);
        if (r == FR_OK)
        {
            r = f_write(&f, line, (UINT)n, &w);
            r = f_close(&f) != FR_OK || w != (UINT)n ? FR_DISK_ERR : r;
        }
    }
    debug_printf("cloud: server configuration deleted (%d), %s %ld\n", (int)r, cfg.ca, d);
    return r == FR_OK || r == FR_NO_FILE ? "ok" : "card";
}

const char *cloud_config_check(char *text)
{
    server_cfg_t c;
    const char *why = config_parse(text, &c);
    return why && strcmp(why, "none") ? why : NULL;
}

/* --- certificate digests --- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void cert_digest(const char *name, int32_t id)
{
    log_args_t a = {0};
    log_arg_num(&a, id);
    long size = flashfs_size(name);
    if (size < 0)
    {
        debug_printf("cloud: %s %ld\n", name, size);
        log_arg_num(&a, (int32_t)size);
        log_fn(LOG_ERROR, 6, 8659, 1, &a); /* cannot be examined */
        return;
    }
    if (!size)
    {
        log_args_t none = {0};
        log_fn(LOG_ERROR, 6, 8375, 1, &none); /* empty: skipped like an engine that does not start */
        return;
    }
    static sha256_t h;
    sha256_init(&h);
    unsigned long off = 0;
    while (off < (unsigned long)size)
    {
        unsigned long k = size - off < sizeof dl_buf ? size - off : sizeof dl_buf;
        long r = flashfs_read(name, off, dl_buf, k);
        if (r != (long)k)
        {
            if (r < 0 && !off)
            {
                debug_printf("cloud: %s %ld\n", name, r);
                log_arg_num(&a, (int32_t)r);
                log_fn(LOG_ERROR, 6, 8658, 1, &a);
                return;
            }
            log_arg_num(&a, (int32_t)(off + (r > 0 ? (unsigned long)r : 0)));
            log_arg_num(&a, (int32_t)size);
            log_fn(LOG_ERROR, 6, 8657, 1, &a);
            return;
        }
        sha256_update(&h, dl_buf, k);
        off += k;
        monitor_mark(ACT_CLOUD);
    }
    uint8_t d[32];
    sha256_final(&h, d);
    debug_verbose("cloud: %s %lu B, %02x%02x%02x%02x\n", name, (unsigned long)size, d[0], d[1], d[2], d[3]);
    /* argument 4 holds bytes 9, 9, 11, 12 (byte 10 not reported) */
    uint8_t w4[4] = {d[8], d[8], d[10], d[11]};
    log_arg_num(&a, (int32_t)be32(d));
    log_arg_num(&a, (int32_t)be32(d + 4));
    log_arg_num(&a, (int32_t)be32(w4));
    for (int i = 12; i < 32; i += 4)
    {
        log_arg_num(&a, (int32_t)be32(d + i));
    }
    log_fn(LOG_INFO, 6, 8656, 0, &a);
}

/* Once per start, before the first API request (the activity takes no job
 * before), at 1 s after start-up or later. The CA is the file the
 * connections use (cfg.ca, /cert/ca.der by default). */
static void cert_digests(void)
{
    while (tick_ms() < 1000)
    {
        vTaskDelay(100);
    }
    cert_digest("/cert/client.der", 8649);
    cert_digest("/cert/private.der", 8650);
    cert_digest(cfg.ca, 8651);
}

/* last result and rolled-back build of the boot state file, when started by
 * the SD bootloader. */
static void boot_state_read(void)
{
    uint8_t bs[32];
    if (bootstate_read(bs))
    {
        ext_set_boot_result(bs[9], le32(bs + 12), bootstate_trial_pending());
    }
}

static void task(void *arg)
{
    (void)arg;
    config_read();
    boot_state_read();
    /* The flash file system only once the network processor's start-up
     * (with its restarts) has ended, which
     * settings_load marks. */
    while (!settings_ready())
    {
        vTaskDelay(100); /* not monitored before the first mark */
    }
#if PORT_CERT_INPUT
    cert_input();
#endif
    cert_digests();
#ifndef CFW_ESP32
    cloud_build_read();
#endif
    for (;;)
    {
        job_t j;
        monitor_mark(ACT_CLOUD); /* also when idle */
        if (xQueueReceive(queue, &j, pdMS_TO_TICKS(1000)) != pdTRUE)
        {
            /* in setup mode, until it ends (no job comes then:
             * no network session); with this activity's buffers. */
            page_run(work, sizeof work, (char *)dl_buf, sizeof dl_buf);
            ext_hello_flush();
            log_post();
            maybe_sync();
#ifdef CFW_ESP32
            update_after_hello();
#endif
            continue;
        }
        job_running = true;
        if (j.kind == JOB_ONLINE && j.session != net_session())
        {
            debug_verbose("cloud: round %lu stale\n", (unsigned long)j.session);
        }
        if (j.kind == JOB_ONLINE && j.session == net_session())
        {
            round_running = true;
            online_round(j.session);
            round_running = false;
        }
        else if (j.kind == JOB_CONTENT)
        {
            dl_cancel = false;
            content_download(&j);
        }
        else if (j.kind == JOB_CLAIM)
        {
            claim(&j);
        }
        else if (j.kind == JOB_UPDATE && j.session == net_session() && cfg.valid)
        {
            monitor_update_round(true);
            update_check(j.session);
            monitor_update_round(false);
        }
        job_running = false;
    }
}

void cloud_init(void)
{
    queue = xQueueCreateStatic(6, sizeof(job_t), queue_buf, &queue_mem);
}

void cloud_start(void)
{
    static StackType_t stack[TASK_STACK(1248)]; /* worst case 3952 B (make stackcheck), +25 % */
    static StaticTask_t tcb;
    xTaskCreateStatic(task, "cloud", TASK_STACK(1248), NULL, 2, stack, &tcb);
}
