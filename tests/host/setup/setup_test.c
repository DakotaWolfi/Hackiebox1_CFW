/*
 * Host test of setup.c: scan list,
 * tokens, hex credentials and the
 * profile slot, the setup page's request parsing.
 * Build and run: tests/host/setup/run.sh
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include "setup.h"

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

static scan_entry_t E(const char *s, int len, int sec, int rssi)
{
    scan_entry_t e;
    memset(&e, 0, sizeof e);
    e.len = (uint8_t)(len < 0 ? (int)strlen(s) : len);
    size_t sl = strlen(s);
    memcpy(e.ssid, s, sl < 32 ? sl : 32);
    e.sec = (uint8_t)sec;
    e.rssi = (int8_t)rssi;
    return e;
}

static bool is(const scan_entry_t *e, const char *s, int sec, int rssi)
{
    return e->len == strlen(s) && !memcmp(e->ssid, s, e->len) && e->sec == sec && e->rssi == rssi;
}

static setup_post_t post(setup_creds_t *c, const char *name, const char *value)
{
    return setup_post(c, (const uint8_t *)name, (unsigned)strlen(name), (const uint8_t *)value,
                      (unsigned)strlen(value));
}

static void scan_tests(void)
{
    scan_entry_t e[30];
    unsigned n = 0;
    e[n++] = E("same", -1, 3, -70);
    e[n++] = E("", 0, 0, -80);
    e[n++] = E("other", -1, 2, -50);
    e[n++] = E("same", -1, 2, -40); /* strongest of "same": its content, first position */
    e[n++] = E("Same", -1, 3, -40); /* case-sensitive */
    e[n++] = E("", 0, 3, -60);      /* hidden: one group */
    e[n++] = E("wep", -1, 1, -30);
    e[n++] = E("same", -1, 0, -40); /* equal RSSI: the earlier one is kept */
    e[n++] = E("toolong", 33, 3, -10);
    n = setup_scan_process(e, n);
    CHECK(n == 4, "count %u", n);
    CHECK(is(&e[0], "same", 2, -40) && is(&e[1], "Same", 3, -40) && is(&e[2], "other", 2, -50) &&
              is(&e[3], "", 3, -60),
          "merge, stable sort, WEP removed");

    n = 0;
    e[n++] = E("a\"b\\c", -1, 3, -50);
    e[n++] = E("0123456789012345678901234567\"89", 31, 3, -51);  /* 31 bytes with a quote: dropped */
    e[n++] = E("0123456789012345678901234567890\\", 32, 3, -52); /* 32 with a backslash: dropped */
    e[n++] = E("abcdefghijklmnopqrstuvwxyzabcdef", 32, 2, -53);  /* 32 letters: kept */
    e[n++] = E("012345678901234567890123456789\"", 31, 3, -54);  /* 30 + quote = 31 -> 32 escaped: dropped */
    e[n++] = E("01234567890123456789012345678\"", 30, 3, -55);   /* 30 -> 31 escaped: kept */
    e[n++] = E("plain", -1, 0, -56);
    n = setup_scan_process(e, n);
    CHECK(n == 4, "escape count %u", n);
    CHECK(n > 0 && e[0].len == 7 && !memcmp(e[0].ssid, "a\\\"b\\\\c", 7), "escaped a\\\"b\\\\c");
    CHECK(n > 1 && e[1].len == 32, "32 letters kept");
    CHECK(n > 2 && e[2].len == 31 && e[2].ssid[29] == '\\', "31-byte escaped kept");
    CHECK(n > 3 && is(&e[3], "plain", 0, -56), "order kept");

    /* first 20 only */
    n = 0;
    for (int i = 0; i < 25; i++)
    {
        char s[16];
        snprintf(s, sizeof s, "n%d", i);
        e[n++] = E(s, -1, 3, -i);
    }
    n = setup_scan_process(e, n);
    CHECK(n == 20 && is(&e[19], "n19", 3, -19), "first 20 entries");

    /* GET tokens */
    uint8_t out[64];
    n = 0;
    e[n++] = E("net", -1, 3, -40);
    e[n++] = E("", 0, 0, -50);
    n = setup_scan_process(e, n);
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_USA", 10, out) == 3 && !memcmp(out, "net", 3), "SSID A");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_UKA", 10, out) == 1 && out[0] == '3', "security A");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_USB", 10, out) == 0, "hidden: empty");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_UKB", 10, out) == 1 && out[0] == '0', "security B");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_USC", 10, out) == 0, "beyond the list");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_UXA", 10, out) == 0, "other token");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_US", 9, out) == 0, "9 characters");
    CHECK(setup_get(e, n, (const uint8_t *)"__SL_G_USU", 10, out) == 0, "letter U");
}

static void token_tests(void)
{
    setup_creds_t c;
    memset(&c, 0, sizeof c);
    setup_creds_clear(&c, 2);
    CHECK(c.language == 2 && c.mode == 0, "preset");
    CHECK(post(&c, "__SL_P_USL", "home") == POST_DONE && c.ssid_len == 4, "SSID");
    CHECK(post(&c, "__SL_P_USL", "123456789012345678901234567890123") == POST_DONE && c.ssid_len == 4 &&
              !memcmp(c.ssid, "home", 4),
          "33-byte SSID ignored, old value stays");
    CHECK(post(&c, "__SL_P_USN", "secretkey") == POST_DONE && c.key_len == 9, "key");
    CHECK(post(&c, "__SL_P_U", "SLtest") == POST_DONE && c.ssid_len == 4, "8-character name ignored silently");
    CHECK(post(&c, "__SL_P_X", "x") == POST_LOGGED, "other prefix logged");
    CHECK(post(&c, "abc", "x") == POST_LOGGED, "short name logged");
    CHECK(post(&c, "__SL_P_UXL", "x") == POST_DONE && c.ssid_len == 4, "9th character not S");
    CHECK(post(&c, "__SL_P_USZ", "x") == POST_LOGGED, "unknown 10th character logged");
    char longname[70];
    memset(longname, 'a', 65);
    memcpy(longname, "__SL_P_USL", 10);
    longname[65] = 0;
    CHECK(post(&c, longname, "x") == POST_DONE && c.ssid_len == 4, "name above 64 ignored silently");
    CHECK(post(&c, "__SL_P_USM", "2") == POST_DONE && c.mode == 2, "mode 2");
    CHECK(post(&c, "__SL_P_USM", "22") == POST_DONE && c.mode == 2, "mode value too long ignored");
    CHECK(post(&c, "__SL_P_USM", "") == POST_DONE && c.mode == 208, "empty mode 208");
    CHECK(post(&c, "__SL_P_UST", "3") == POST_DONE && c.language == 3, "language 3");
    CHECK(post(&c, "__SL_P_UST", "4") == POST_LOGGED && c.language == 3, "language 4 logged");
    CHECK(post(&c, "__SL_P_UST", "12") == POST_LOGGED, "language of 2 characters logged");
    CHECK(post(&c, "__SL_P_USO", "") == POST_SUBMIT, "submit");
    uint8_t z[] = {'a', 'b', 0, 'c'};
    setup_post(&c, (const uint8_t *)"__SL_P_USL", 10, z, 4);
    CHECK(c.ssid_len == 2, "SSID up to the zero byte");
    setup_creds_clear(&c, 0);
    CHECK(c.mode == 208 && !c.ssid_len && !c.key_len, "mode kept over a new session");
    CHECK(setup_join_security(0) == 0 && setup_join_security(1) == 1 && setup_join_security(3) == 2 &&
              setup_join_security(7) == 2 && setup_join_security(208) == 2,
          "join security");
    CHECK(setup_profile_security(2) == 2 && setup_profile_security(7) == 0 && setup_profile_security(1) == 1,
          "profile security");
}

static void hex_tests(void)
{
    setup_creds_t c;
    memset(&c, 0, sizeof c);
    /* 32-byte key as 63 + 1 digits */
    char k[65];
    for (int i = 0; i < 64; i++)
    {
        k[i] = "0123456789abcdef"[i % 16];
    }
    k[64] = 0;
    char p1[64];
    memcpy(p1, k, 63);
    p1[63] = 0;
    post(&c, "__SL_P_USP", p1);
    post(&c, "__SL_P_USQ", k + 63);
    post(&c, "__SL_P_USR", "4142");
    CHECK(setup_form(&c) && c.key_len == 32 && c.key[0] == 0x01 && c.key[31] == 0xef && c.ssid_len == 2 &&
              !memcmp(c.ssid, "AB", 2),
          "key from 63 + 1 digits, SSID from hex");
    /* text takes precedence */
    memset(&c, 0, sizeof c);
    post(&c, "__SL_P_USN", "text");
    post(&c, "__SL_P_USP", "4142");
    post(&c, "__SL_P_USL", "s");
    CHECK(setup_form(&c) && c.key_len == 4 && !memcmp(c.key, "text", 4), "text first");
    /* odd number of digits */
    memset(&c, 0, sizeof c);
    post(&c, "__SL_P_USL", "s");
    post(&c, "__SL_P_USP", "414");
    CHECK(!setup_form(&c) && !c.key_len, "odd digits fail");
    /* a hex sequence with an invalid digit after a pair */
    memset(&c, 0, sizeof c);
    post(&c, "__SL_P_USN", "abcdef");
    post(&c, "__SL_P_USN", "");
    post(&c, "__SL_P_USP", "41zz");
    post(&c, "__SL_P_USQ", "");
    post(&c, "__SL_P_USL", "net");
    CHECK(!setup_form(&c) && !c.key_len, "invalid digit after a pair");
    CHECK(!setup_form(&c) && !c.key_len, "submit again: still nothing");
    /* second part without a full first part */
    memset(&c, 0, sizeof c);
    post(&c, "__SL_P_USL", "s");
    post(&c, "__SL_P_USP", "41");
    post(&c, "__SL_P_USQ", "42");
    CHECK(!setup_form(&c), "second part with a short first part");
    /* SSID above 32 bytes */
    memset(&c, 0, sizeof c);
    char h[64];
    memset(h, '4', 63);
    h[63] = 0;
    post(&c, "__SL_P_USR", h);
    post(&c, "__SL_P_USS", "141"); /* 66 digits = 33 bytes */
    CHECK(!setup_form(&c) && !c.ssid_len, "SSID of 33 bytes fails");
    /* empty everything: empty values */
    memset(&c, 0, sizeof c);
    CHECK(setup_form(&c) && !c.key_len && !c.ssid_len, "empty text from empty parts");
}

static slot_info_t S(const char *ssid, int sec)
{
    slot_info_t s;
    memset(&s, 0, sizeof s);
    if (ssid)
    {
        s.present = true;
        s.len = (uint8_t)strlen(ssid);
        memcpy(s.ssid, ssid, s.len);
        s.sec = (uint8_t)sec;
    }
    return s;
}

static void slot_tests(void)
{
    slot_info_t s[7];
    slot_plan_t p;
    for (int i = 0; i < 7; i++)
    {
        s[i] = S(NULL, 0);
    }
    setup_slot_plan(s, (const uint8_t *)"N1", 2, 2, &p);
    CHECK(!p.match && p.empty == 0 && p.expect == 0 && p.del_after == 1 && p.n_before == 0, "empty box");
    s[0] = S("N1", 2);
    s[1] = S("N2", 2);
    s[2] = S("N3", 2);
    s[3] = S("N4", 2);
    s[4] = S("N5", 2);
    s[5] = S("N6", 2);
    setup_slot_plan(s, (const uint8_t *)"N3", 2, 2, &p);
    CHECK(p.match && p.expect == 2 && p.n_before == 0 && p.del_after == -1, "N3 again: replaced in place");
    setup_slot_plan(s, (const uint8_t *)"N7", 2, 2, &p);
    CHECK(!p.match && p.empty == 6 && p.del_after == 0 && p.expect == 6, "N7 into slot 6, slot 0 deleted");
    s[6] = S("N7", 2);
    s[0] = S(NULL, 0);
    setup_slot_plan(s, (const uint8_t *)"N4", 2, 0, &p);
    CHECK(p.match && p.n_before == 1 && p.del_before[0] == 3 && p.expect == 3 && p.del_after == -1,
          "N4 open: the WPA2 profile deleted");
    for (int i = 0; i < 7; i++)
    {
        s[i] = S("X", 2);
    }
    s[2].ssid[0] = 'Y';
    s[4].ssid[0] = 'Y';
    s[4].sec = 0;
    setup_slot_plan(s, (const uint8_t *)"Y", 1, 0, &p);
    CHECK(p.match && p.n_before == 1 && p.del_before[0] == 2 && p.expect == 4, "two matches, kept by security");
    setup_slot_plan(s, (const uint8_t *)"Z", 1, 2, &p);
    CHECK(!p.match && p.n_before == 1 && p.del_before[0] == 0 && p.expect == 0 && p.del_after == 1, "full");
    s[1].len = 40; /* reported above 32: does not match, holds a profile */
    setup_slot_plan(s, (const uint8_t *)"X", 1, 2, &p);
    CHECK(p.match && p.expect == 0 && p.n_before == 3 && p.del_before[0] == 3, "long SSID slot ignored (%u deletes)", p.n_before);
    s[0].bssid = true;
    setup_slot_plan(s, (const uint8_t *)"X", 1, 2, &p);
    CHECK(p.expect == 3 && p.del_before[0] == 0, "a profile with BSSID is not kept");
}

static int head(const char *req, http_req_t *r, char *buf)
{
    size_t n = strlen(req);
    memcpy(buf, req, n);
    return setup_http_head(buf, (unsigned)n, r);
}

static void http_tests(void)
{
    char b[600];
    http_req_t r;
    const char *g = "GET /scan.txt?x=1 HTTP/1.1\r\nHost: 192.168.1.1\r\n\r\n";
    CHECK(head(g, &r, b) == 1 && !r.post && !strcmp(r.path, "/scan.txt") && r.length == 0 && r.head == strlen(g),
          "GET with query");
    for (size_t k = 0; k < strlen(g); k++)
    {
        memcpy(b, g, k);
        CHECK(setup_http_head(b, (unsigned)k, &r) == 0, "incomplete head (%zu bytes): more needed", k);
    }
    const char *p = "POST /cert/2 HTTP/1.1\r\nhost: x\r\ncontent-length:  1234\r\n\r\n\x30\x82";
    CHECK(head(p, &r, b) == 1 && r.post && !strcmp(r.path, "/cert/2") && r.length == 1234 &&
              !memcmp(b + r.head, "\x30\x82", 2),
          "POST, lower-case Content-Length, body after the head");
    CHECK(head("POST /config.txt HTTP/1.1\r\nContent-Length: 5\r\n\r\n", &r, b) == 1 && r.length == 5 &&
              !strcmp(r.path, "/config.txt"),
          "Content-Length");
    CHECK(head("POST /wifi HTTP/1.1\r\n\r\nContent-Length: 5", &r, b) == 1 && r.length == 0,
          "Content-Length in the body does not count");
    CHECK(head("GET / HTTP/1.0\r\n\r\n", &r, b) == 1 && !strcmp(r.path, "/"), "GET /");
    CHECK(head("GET\r\n\r\n", &r, b) == -1 && head("GET x HTTP/1.1\r\n\r\n", &r, b) == -1, "malformed");

    char f[] = "__SL_P_USL=&__SL_P_USR=4142&&__SL_P_USM=3&__SL_P_USO";
    char *q = f, *end = f + strlen(f), *n, *v;
    unsigned nl, vl, i = 0;
    const char *want[][2] = {{"__SL_P_USL", ""}, {"__SL_P_USR", "4142"}, {"", ""}, {"__SL_P_USM", "3"}, {"__SL_P_USO", ""}};
    while (setup_form_field(&q, end, &n, &nl, &v, &vl))
    {
        CHECK(i < 5 && nl == strlen(want[i][0]) && !memcmp(n, want[i][0], nl) && vl == strlen(want[i][1]) &&
                  !memcmp(v, want[i][1], vl),
              "form field %u", i);
        i++;
    }
    CHECK(i == 5, "5 fields (%u)", i);

    /* the fields as POST tokens: a Wi-Fi submit from the page */
    setup_creds_t c;
    memset(&c, 0, sizeof c);
    char w[] = "__SL_P_USL=&__SL_P_USN=&__SL_P_USR=6869&__SL_P_USS=&__SL_P_USP=6B6579&__SL_P_USQ=&__SL_P_USM=3&"
               "__SL_P_USO=";
    setup_post_t last = POST_DONE;
    q = w;
    end = w + strlen(w);
    while (setup_form_field(&q, end, &n, &nl, &v, &vl))
    {
        last = setup_post(&c, (uint8_t *)n, nl, (uint8_t *)v, vl);
    }
    CHECK(last == POST_SUBMIT && setup_form(&c) && c.ssid_len == 2 && !memcmp(c.ssid, "hi", 2) && c.key_len == 3 &&
              !memcmp(c.key, "key", 3) && setup_join_security(c.mode) == 2,
          "page submit: SSID and key from the hex tokens, WPA2");
}

int main(void)
{
    http_tests();
    scan_tests();
    token_tests();
    hex_tests();
    slot_tests();
    printf("setup test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
