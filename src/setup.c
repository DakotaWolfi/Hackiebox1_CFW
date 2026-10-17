/*
 * Setup mode data rules (see setup.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "setup.h"

/* --- scan list --- */

static bool same_ssid(const scan_entry_t *a, const scan_entry_t *b)
{
    return a->len == b->len && !memcmp(a->ssid, b->ssid, a->len);
}

unsigned setup_scan_process(scan_entry_t *e, unsigned n)
{
    if (n > SETUP_SCAN_MAX)
    {
        n = SETUP_SCAN_MAX;
    }
    /* entries with an SSID length above 32 are left out. */
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++)
    {
        if (e[i].len <= 32)
        {
            e[m++] = e[i];
        }
    }
    n = m;
    /* one entry per SSID, the strongest in the place of the
     * group's first entry; hidden networks form one group. */
    m = 0;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned j = 0;
        while (j < m && !same_ssid(&e[j], &e[i]))
        {
            j++;
        }
        if (j == m)
        {
            e[m++] = e[i];
        }
        else if (e[i].rssi > e[j].rssi)
        {
            e[j] = e[i];
        }
    }
    n = m;
    /* stable sort, strongest first. */
    for (unsigned i = 1; i < n; i++)
    {
        scan_entry_t t = e[i];
        unsigned j = i;
        while (j > 0 && e[j - 1].rssi < t.rssi)
        {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = t;
    }
    /* escape '"' and '\'; an entry whose escaped SSID would
     * reach 32 bytes is left out. */
    m = 0;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned ins = 0;
        for (unsigned k = 0; k < e[i].len; k++)
        {
            if (e[i].ssid[k] == '"' || e[i].ssid[k] == '\\')
            {
                ins++;
            }
        }
        if (ins && e[i].len + ins >= 32)
        {
            continue;
        }
        if (ins)
        {
            uint8_t t[32];
            unsigned d = 0;
            for (unsigned k = 0; k < e[i].len; k++)
            {
                if (e[i].ssid[k] == '"' || e[i].ssid[k] == '\\')
                {
                    t[d++] = '\\';
                }
                t[d++] = e[i].ssid[k];
            }
            memcpy(e[i].ssid, t, d);
            e[i].len = (uint8_t)d;
        }
        e[m++] = e[i];
    }
    n = m;
    /* WEP networks removed. */
    m = 0;
    for (unsigned i = 0; i < n; i++)
    {
        if (e[i].sec != 1)
        {
            e[m++] = e[i];
        }
    }
    return m;
}

/* --- tokens --- */

unsigned setup_get(const scan_entry_t *list, unsigned n, const uint8_t *name, unsigned name_len, uint8_t *out)
{
    if (name_len < 10 || name[9] < 'A' || name[9] > 'T')
    {
        return 0;
    }
    unsigned i = (unsigned)(name[9] - 'A');
    if (i >= n)
    {
        return 0;
    }
    if (name[8] == 'S')
    {
        memcpy(out, list[i].ssid, list[i].len);
        return list[i].len;
    }
    if (name[8] == 'K')
    {
        out[0] = (uint8_t)('0' + list[i].sec % 10);
        return 1;
    }
    return 0;
}

void setup_creds_clear(setup_creds_t *c, uint8_t language)
{
    uint8_t mode = c->mode;
    memset(c, 0, sizeof *c);
    c->mode = mode;
    c->language = language;
}

/* the bytes up to the first zero byte. */
static uint8_t text_len(const uint8_t *v, unsigned len)
{
    unsigned n = 0;
    while (n < len && v[n])
    {
        n++;
    }
    return (uint8_t)n;
}

setup_post_t setup_post(setup_creds_t *c, const uint8_t *name, unsigned name_len, const uint8_t *value,
                        unsigned value_len)
{
    if (name_len > 64)
    {
        return POST_DONE;
    }
    if (name_len < 8 || memcmp(name, "__SL_P_U", 8))
    {
        return POST_LOGGED;
    }
    if (name_len < 10 || name[8] != 'S')
    {
        return POST_DONE;
    }
    switch (name[9])
    {
    case 'L':
        if (value_len <= 32)
        {
            c->ssid_len = text_len(value, value_len);
            memcpy(c->ssid, value, c->ssid_len);
        }
        return POST_DONE;
    case 'N':
        if (value_len <= 64)
        {
            c->key_len = text_len(value, value_len);
            memcpy(c->key, value, c->key_len);
        }
        return POST_DONE;
    case 'R':
    case 'S':
    case 'P':
    case 'Q':
    {
        unsigned i = name[9] == 'R' ? 0 : name[9] == 'S' ? 1
                                      : name[9] == 'P'   ? 2
                                                         : 3;
        if (value_len <= 63)
        {
            memcpy(c->hex[i], value, value_len);
            c->hex_len[i] = (uint8_t)value_len;
        }
        return POST_DONE;
    }
    case 'M':
        /* an empty value gives 0 - 48 = 208. */
        if (value_len <= 1)
        {
            c->mode = (uint8_t)((value_len ? value[0] : 0) - '0');
        }
        return POST_DONE;
    case 'T':
        if (value_len == 1 && value[0] >= '0' && value[0] <= '3')
        {
            c->language = (uint8_t)(value[0] - '0');
            return POST_DONE;
        }
        return POST_LOGGED;
    case 'O':
        return POST_SUBMIT;
    default:
        return POST_LOGGED;
    }
}

static int nibble(char h)
{
    if (h >= '0' && h <= '9')
    {
        return h - '0';
    }
    if (h >= 'A' && h <= 'F')
    {
        return h - 'A' + 10;
    }
    if (h >= 'a' && h <= 'f')
    {
        return h - 'a' + 10;
    }
    return -1;
}

/* The value rules 1 to 5 for one value. */
static bool form(const char *a, unsigned an, const char *b, unsigned bn, uint8_t *out, uint8_t *out_len,
                 unsigned max)
{
    char t[126];
    unsigned n = an;
    if (an < 63 && bn)
    {
        return false;
    }
    memcpy(t, a, an);
    if (an == 63)
    {
        memcpy(t + an, b, bn);
        n += bn;
    }
    if (n % 2 || n / 2 > max)
    {
        return false;
    }
    uint8_t v[64];
    for (unsigned i = 0; i < n; i += 2)
    {
        int h = nibble(t[i]), l = nibble(t[i + 1]);
        if (h < 0 || l < 0)
        {
            return false;
        }
        v[i / 2] = (uint8_t)(h << 4 | l);
    }
    memcpy(out, v, n / 2);
    *out_len = (uint8_t)(n / 2);
    return true;
}

bool setup_form(setup_creds_t *c)
{
    if (!c->key_len && !form(c->hex[2], c->hex_len[2], c->hex[3], c->hex_len[3], c->key, &c->key_len, 64))
    {
        return false;
    }
    if (!c->ssid_len && !form(c->hex[0], c->hex_len[0], c->hex[1], c->hex_len[1], c->ssid, &c->ssid_len, 32))
    {
        return false;
    }
    return true;
}

uint8_t setup_join_security(uint8_t mode)
{
    return mode == 0 ? 0 : mode == 1 ? 1
                                     : 2;
}

uint8_t setup_profile_security(uint8_t mode)
{
    return mode == 0 ? 0 : mode == 1 ? 1
                       : mode <= 3   ? 2
                                     : 0;
}

/* --- profile slot --- */

void setup_slot_plan(const slot_info_t s[7], const uint8_t *ssid, uint8_t len, uint8_t sec,
                     slot_plan_t *p)
{
    memset(p, 0, sizeof *p);
    p->del_after = p->expect = p->empty = -1;
    int kept = -1, first_match = -1;
    for (int i = 0; i < 7; i++)
    {
        bool m = s[i].present && s[i].len <= 32 && s[i].len == len && !memcmp(s[i].ssid, ssid, len);
        if (m && first_match < 0)
        {
            first_match = i;
        }
        if (m && kept < 0 && s[i].sec == sec && !s[i].bssid)
        {
            kept = i;
        }
        if (!s[i].present && p->empty < 0)
        {
            p->empty = (int8_t)i;
        }
    }
    if (first_match >= 0)
    {
        p->match = true;
        for (int i = 0; i < 7; i++)
        {
            bool m = s[i].present && s[i].len <= 32 && s[i].len == len && !memcmp(s[i].ssid, ssid, len);
            if (m && i != kept)
            {
                p->del_before[p->n_before++] = (int8_t)i;
            }
        }
        p->expect = (int8_t)(kept >= 0 ? kept : first_match);
    }
    else if (p->empty >= 0)
    {
        p->expect = p->empty;
        p->del_after = (int8_t)((p->empty + 1) % 7);
    }
    else
    {
        p->del_before[p->n_before++] = 0;
        p->expect = 0;
        p->del_after = 1;
    }
}

/* --- setup page requests --- */

int setup_http_head(char *buf, unsigned n, http_req_t *r)
{
    buf[n] = 0;
    char *e = strstr(buf, "\r\n\r\n"), *p = strchr(buf, ' '), *l;
    if (!e)
    {
        return 0;
    }
    r->head = (unsigned)(e + 4 - buf);
    r->post = *buf == 'P';
    l = strstr(buf, "ength:"); /* Content-Length, any case of the C and L */
    r->length = l && l < e ? (unsigned)strtoul(l + 6, NULL, 10) : 0;
    if (!p || p[1] != '/')
    {
        return -1;
    }
    r->path = ++p;
    while (*p > ' ' && *p != '?')
    {
        p++;
    }
    *p = 0;
    return 1;
}

bool setup_form_field(char **p, char *end, char **name, unsigned *nl, char **val, unsigned *vl)
{
    if (*p >= end)
    {
        return false;
    }
    char *f = *p, *a = memchr(f, '&', (size_t)(end - f)), *eq;
    if (!a)
    {
        a = end;
    }
    *p = a + 1;
    eq = memchr(f, '=', (size_t)(a - f));
    if (!eq)
    {
        eq = a;
    }
    *name = f;
    *nl = (unsigned)(eq - f);
    *val = eq + (eq < a);
    *vl = (unsigned)(a - *val);
    return true;
}
