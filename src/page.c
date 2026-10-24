/*
 * Setup page. How the page is served is implementation-
 * defined; here the firmware serves it itself, because the network
 * processor's token protocol takes no file upload:
 *
 *   GET  (other paths)   the file /cfw/setup.htm of the card, streamed
 *   GET  /scan.txt       the scan list, one line per entry:
 *                        security digit, space, escaped SSID in hex
 *   GET  /config.txt     the file /cfw/config.txt (empty when missing)
 *   GET  /lang.txt       the box language, one digit 0 to 3 (the setup
 *                        language is preset to it)
 *   GET  /version.txt    the firmware's version text, CFW_<version>: the
 *                        page shows it next to its own version and the
 *                        firmware it needs
 *   POST /wifi           form fields named as the POST tokens of the
 *                        setup mode, handled exactly as those
 *   POST /lang.txt       a new box language, one digit 0 to 3, stored at
 *                        once without touching Wi-Fi
 *   POST /config.txt     the new /cfw/config.txt (up to 1024 bytes): the
 *                        page keeps the other lines and sets host and
 *                        port; stored only when it passes the check of
 *                        the start
 *   POST /reset/wifi, /reset/content, /reset/server
 *                        delete every Wi-Fi profile; every content but the
 *                        sound prompts, the trash and the extension's
 *                        files; /cfw/config.txt with its CA file
 *                        (the page asks first)
 *   POST /rescan         a new search for networks: answered first, then
 *                        the access point closes for the search
 *   POST /cert/0, /cert/1, /cert/2
 *                        private.der, client.der, ca.der as the raw body,
 *                        checked and stored in the flash
 *
 * Answers to the POSTs are text/plain: "ok", or a word for the reason
 * ("bad" request, "der" not one DER element, "flash" write failed, "host"
 * invalid, "vendor" domain, "port" invalid, "long" over 1024 bytes,
 * "card" write failed); the page shows the text for it. Nothing is
 * stored on a refusal.
 * Without /cfw/setup.htm on the card the server does not start and the
 * network processor keeps serving the vendor page (/www/index.html, left
 * unchanged) with the token protocol.
 *
 * It runs in the cloud activity, which has no work in setup mode (no
 * network session), with that activity's buffers: no other
 * code uses them while page_run runs. The listening socket is
 * non-blocking and polled, and a request socket waits at most 1 s per
 * receive, so the net activity's hand-over (osi_driver_busy) and
 * net_page_on() are seen within about a second.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "cloud.h"
#include "debug.h"
#include "monitor.h"
#include "net.h"
#include "sdfiles.h"
#include "page.h"
#include "settings.h"
#include "setup.h"
#include "sock_port.h"
#include "tick.h"
#include "version.h"

#define PAGE_FILE "/cfw/setup.htm"

static bool send_all(int16_t s, const void *d, unsigned n)
{
    const uint8_t *p = d;
    while (n)
    {
        long r = sock_send(s, p, n > 1400 ? 1400 : n);
        monitor_mark(ACT_CLOUD);
        if (r <= 0 && (r != SOCK_AGAIN || !net_page_on()))
        {
            return false;
        }
        if (r > 0)
        {
            p += r;
            n -= (unsigned)r;
        }
    }
    return true;
}

/* The answer's head (HTTP/1.0: the body ends with the connection), then
 * the text, if any. */
static void reply(int16_t s, char *b, const char *type, const char *text)
{
    int k = snprintf(b, 128, "HTTP/1.0 200 OK\r\nContent-Type: text/%s\r\n\r\n", type);
    if (send_all(s, b, (unsigned)k) && text)
    {
        send_all(s, text, strlen(text));
    }
}

/* Some bytes (up to n) into d: their number, or <= 0 after a pause of
 * 5 s, an error or the end of setup mode. */
static long rx(int16_t s, void *d, unsigned n)
{
    uint32_t t0 = tick_ms();
    for (;;)
    {
        long k = sock_recv(s, d, n > 1400 ? 1400 : n); /* 1 s receive timeout */
        monitor_mark(ACT_CLOUD);
        if (k != SOCK_AGAIN || !net_page_on() || tick_ms() - t0 >= 5000)
        {
            return k;
        }
    }
}

/* Until d holds want bytes (have there already). */
static bool fill(int16_t s, uint8_t *d, unsigned have, unsigned want)
{
    while (have < want)
    {
        long k = rx(s, d + have, want - have);
        if (k <= 0)
        {
            return false;
        }
        have += (unsigned)k;
    }
    return true;
}

/* the new /cfw/config.txt (n bytes in w), checked as at start; the page
 * keeps the other lines and puts host and port in. "ok" when written, else the reason. */
static const char *config_write(char *w, unsigned n)
{
    char *t = w + 1100;
    FIL f;
    UINT wr = 0;
    if (n > 1024)
    {
        return "long";
    }
    memcpy(t, w, n);
    t[n] = 0;
    const char *why = cloud_config_check(t);
    if (why)
    {
        return why;
    }
    f_mkdir("/cfw");
    if (f_open(&f, "/cfw/config.tmp", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return "card";
    }
    FRESULT e = f_write(&f, w, n, &wr);
    if ((f_close(&f) | e) != FR_OK || wr != n)
    {
        return "card";
    }
    f_unlink("/cfw/config.txt");
    return f_rename("/cfw/config.tmp", "/cfw/config.txt") ? "card" : "ok";
}

static void handle(int16_t s, uint8_t *work, unsigned wn, char *buf, unsigned bn)
{
    http_req_t r = {0};
    unsigned have = 0;
    int h;
    const char *res = "bad";
    while (!(h = setup_http_head(buf, have, &r)) && have < bn - 1)
    {
        long k = rx(s, buf + have, bn - 1 - have);
        if (k <= 0)
        {
            return;
        }
        have += (unsigned)k;
    }
    char *p = h > 0 ? r.path : "?", *w = (char *)work;
    unsigned got = have - r.head, n = r.length;
    if (h > 0 && !r.post)
    {
        debug_printf("page: GET %s\n", p);
        if (!strcmp(p, "/scan.txt"))
        {
            /* security digit and the escaped SSID in hex */
            const scan_entry_t *e;
            unsigned m = net_scan_list(&e);
            n = 0;
            for (unsigned i = 0; i < m; i++)
            {
                w[n++] = (char)('0' + e[i].sec % 10);
                w[n++] = ' ';
                for (unsigned k = 0; k < e[i].len; k++)
                {
                    w[n++] = "0123456789ABCDEF"[e[i].ssid[k] >> 4];
                    w[n++] = "0123456789ABCDEF"[e[i].ssid[k] & 15];
                }
                w[n++] = '\n';
            }
            reply(s, buf, "plain", NULL);
            send_all(s, w, n);
        }
        else if (!strcmp(p, "/lang.txt"))
        {
            char l[2] = {(char)('0' + settings_language() % 10), 0};
            reply(s, buf, "plain", l);
        }
        else if (!strcmp(p, "/version.txt"))
        {
            reply(s, buf, "plain", CFW_VERSION_STRING);
        }
        else
        {
            /* the page, or the server configuration to fill in */
            FIL f;
            bool page = strcmp(p, "/config.txt");
            reply(s, buf, page ? "html" : "plain", NULL);
            if (f_open(&f, page ? PAGE_FILE : "/cfw/config.txt", FA_READ) == FR_OK)
            {
                while (f_read(&f, w, wn, &n) == FR_OK && n && send_all(s, w, n))
                    ;
                f_close(&f);
            }
        }
        return;
    }
    /* POST: the body (up to 4096 bytes) into the work buffer */
    if (h > 0 && got <= n && n <= wn)
    {
        memcpy(w, buf + r.head, got);
        if (!fill(s, work, got, n))
        {
            return;
        }
        if (!strncmp(p, "/cert/", 6))
        {
            /* check and store a certificate */
            unsigned i = (unsigned)(p[6] - '0');
            int e = i < 3 && !p[7] ? cloud_cert_put(i, n) : -1;
            res = e < 0 ? "der" : e ? "flash"
                                    : "ok";
        }
        else if (!strcmp(p, "/wifi"))
        {
            /* Wi-Fi: the fields as the POST tokens of the setup mode, one
             * after the other */
            char *f = w, *end = w + n, *k, *v;
            unsigned kl, vl;
            while (setup_form_field(&f, end, &k, &kl, &v, &vl))
            {
                net_setup_field((const uint8_t *)k, kl, (const uint8_t *)v, vl);
            }
            res = "ok";
        }
        else if (!strcmp(p, "/lang.txt"))
        {
            /* the language alone, as the language token of a join would
             * store it */
            if (n == 1 && w[0] >= '0' && w[0] <= '3')
            {
                settings_set_language((uint8_t)(w[0] - '0'));
                res = settings_language() == (uint8_t)(w[0] - '0') ? "ok" : "flash";
            }
        }
        else if (!strcmp(p, "/config.txt"))
        {
            res = config_write(w, n);
        }
        else if (!strcmp(p, "/reset/wifi"))
        {
            res = net_profiles_delete() < 0 ? "flash" : "ok";
        }
        else if (!strcmp(p, "/reset/content"))
        {
            sd_delete_content();
            res = "ok";
        }
        else if (!strcmp(p, "/reset/server"))
        {
            res = cloud_server_forget();
        }
        else if (!strcmp(p, "/rescan"))
        {
            res = "ok";
        }
    }
    bool rescan = !strcmp(res, "ok") && !strcmp(p, "/rescan");
    debug_printf("page: %s -> %s\n", p, res);
    reply(s, buf, "plain", res);
    if (rescan)
    {
        net_setup_rescan(); /* after the answer: the search ends this page server */
    }
}

void page_run(uint8_t *work, unsigned wn, char *buf, unsigned bn)
{
    FILINFO fi;
    if (!net_page_begin())
    {
        return;
    }
    if (f_stat(PAGE_FILE, &fi) != FR_OK)
    {
        net_page_end(false);
        return;
    }
    /* The network processor's HTTP server gives port 80 free. */
    static long last = 1; /* result of the last try, shown when it changes */
    long st = sock_http_stop();
    int16_t l = sock_listen(80);
    long r = l >= 0 ? 0 : l;
    if (r != last || r >= 0)
    {
        debug_printf("page: http stop %ld, port 80 %ld\n", st, r);
    }
    last = r;
    while (l >= 0 && net_page_on())
    {
        monitor_mark(ACT_CLOUD);
        int16_t s = sock_accept(l);
        if (s < 0)
        {
            if (s != SOCK_AGAIN)
            {
                /* the listening socket is broken: made again */
                debug_printf("page: accept %d\n", s);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        handle(s, work, wn, buf, bn);
        sock_close(s);
    }
    if (l >= 0)
    {
        sock_close(l);
        debug_printf("page: stopped\n");
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* tried again from the activity's loop */
    }
    net_page_end(true);
}
