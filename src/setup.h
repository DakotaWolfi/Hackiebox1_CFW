/*
 * Setup mode: the data rules without the network processor:
 * scan-list processing, the token
 * protocol of the setup page, the credentials, the choice of the profile slot,
 * and the request parsing of the setup page.
 * Pure functions, tested on the host (tests/host/setup).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SETUP_H
#define SETUP_H

#include <stdbool.h>
#include <stdint.h>

#define SETUP_SCAN_MAX 20

/* One scan-list entry; after processing, ssid holds the escaped SSID. */
typedef struct scan_entry
{
    uint8_t ssid[32];
    uint8_t len;
    uint8_t sec; /* scan security type: 0 open, 1 WEP, 2 WPA, 3 WPA2 */
    int8_t rssi;
} scan_entry_t;

/* Scan list processing (first 20, SSID length above 32 left out);
 * returns the number of entries left at the start of e. */
unsigned setup_scan_process(scan_entry_t *e, unsigned n);

/* GET token: writes the value to out (up to 32 bytes) and
 * returns its length. */
unsigned setup_get(const scan_entry_t *list, unsigned n, const uint8_t *name, unsigned name_len,
                   uint8_t *out);

/* Values received from the setup page. */
typedef struct
{
    uint8_t ssid[32], ssid_len; /* text value up to its first zero byte */
    uint8_t key[64], key_len;
    char hex[4][63]; /* R, S (SSID), P, Q (key) */
    uint8_t hex_len[4];
    uint8_t mode;     /* security mode, kept until the firmware restarts */
    uint8_t language; /* setup language */
} setup_creds_t;

/* Clears all but the security mode, presets the
 * language. */
void setup_creds_clear(setup_creds_t *c, uint8_t language);

typedef enum
{
    POST_DONE,   /* stored, or ignored without a log record */
    POST_LOGGED, /* ignored with a log record */
    POST_SUBMIT, /* the submit token */
} setup_post_t;

/* POST token. */
setup_post_t setup_post(setup_creds_t *c, const uint8_t *name, unsigned name_len, const uint8_t *value,
                        unsigned value_len);

/* On submit: forms empty text values from the hex parts (key
 * first, then SSID); false on a decoding failure. */
bool setup_form(setup_creds_t *c);

/* SimpleLink security types (0 open, 1 WEP, 2 WPA/WPA2) for
 * the join attempt and for the stored profile. */
uint8_t setup_join_security(uint8_t mode);
uint8_t setup_profile_security(uint8_t mode);

/* A profile slot as read. */
typedef struct
{
    bool present; /* the read succeeded */
    uint8_t len;  /* reported SSID length */
    uint8_t ssid[32];
    uint8_t sec;
    bool bssid; /* a BSSID is set */
} slot_info_t;

/* The profile commands: delete del_before[0..n_before-1] in
 * that order, add the profile, then delete del_after (-1: none) unless the
 * add reported that slot. expect is the slot the add should report. */
typedef struct
{
    int8_t del_before[7];
    uint8_t n_before;
    int8_t del_after;
    int8_t expect;
    int8_t empty; /* the lowest empty slot k (record 7172), -1 if none */
    bool match;   /* a matching slot was found (record 7184) */
} slot_plan_t;

void setup_slot_plan(const slot_info_t s[7], const uint8_t *ssid, uint8_t len, uint8_t sec,
                     slot_plan_t *p);

/* --- setup page requests (page.c) --- */

/* An HTTP request head at the start of buf. */
typedef struct
{
    bool post;       /* POST, else GET (by the first letter) */
    char *path;      /* terminated in buf, without the query */
    unsigned length; /* Content-Length, 0 when absent */
    unsigned head;   /* bytes up to and including the empty line */
} http_req_t;

/* 1 complete (r filled, buf changed), 0 more bytes needed (n bytes so
 * far; buf holds n + 1), -1 malformed. Content-Length is found as
 * "ength:" in the head; more than the buffer is refused by the caller. */
int setup_http_head(char *buf, unsigned n, http_req_t *r);

/* Next field name=value of a form body at *p (up to end), '&' separated;
 * the values are taken as sent (no percent decoding: the page sends text
 * in the hex tokens). false when none is left. */
bool setup_form_field(char **p, char *end, char **name, unsigned *nl, char **val, unsigned *vl);

#endif
