/*
 * Requests to the API host with the server configuration of
 * one activity that makes the requests one at a time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CLOUD_H
#define CLOUD_H

#include <stdbool.h>
#include <stdint.h>

/* Cloud errors. */
typedef enum
{
    CE_OK = 0,
    CE_NAME,        /* name resolution */
    CE_CONNECT,     /* connection not established */
    CE_UNAVAILABLE, /* 503 */
    CE_REFUSED,     /* content refused */
    CE_RANGE,       /* range not satisfiable */
    CE_STATUS,      /* unexpected status */
    CE_PROTOCOL,
    CE_TIMEOUT,
    CE_TRANSFER,
    CE_CANCELLED, /* Wi-Fi change, cancelled */
} cloud_error_t;

/* Reads /cfw/config.txt; call once after the card is mounted,
 * then starts the activity. */
void cloud_init(void); /* before the activities start */
void cloud_start(void);
/* A valid server configuration is in effect; after the
 * activity has read it. */
bool cloud_configured(void);

/* Online round for network session s. */
void cloud_online_round(uint32_t s);

const char *cloud_error_name(cloud_error_t e);

/* State of the content download, read by the
 * player. */
typedef struct
{
    volatile uint32_t serial; /* request number; a new request cancels */
    volatile bool active;     /* requested and not ended */
    volatile bool done;       /* complete on the card */
    volatile bool failed;
    volatile bool data; /* data has arrived */
    volatile cloud_error_t error;
    volatile int status;
    volatile uint32_t readable; /* file offset up to which the card holds the data */
    volatile uint32_t pos;      /* bytes written so far, from the start of the file */
    volatile uint32_t size;     /* Z */
    uint8_t uid[8];
} cloud_download_t;
extern cloud_download_t cloud_dl;

/* Requests the content of uid from offset; token NULL for
 * v1. if_range is the audio_id for a resumed request. Cancels a running
 * download. */
void cloud_content(const uint8_t uid[8], const uint8_t *token, uint32_t offset, uint32_t if_range);

/* A download of uid runs and has not failed: placing the figure again
 * follows it instead of requesting anew. */
bool cloud_content_running(const uint8_t uid[8]);

/* Stops the running content download (it ends as cancelled). */
void cloud_content_cancel(void);

/* True while the activity has the download file open (from its preparation
 * until it is closed); the player waits for false before it moves that
 * file to the trash. */
bool cloud_download_file_open(void);

/* true (once, then cleared) when a socket operation failed for
 * lack of buffers in the network processor; the network activity then
 * restarts the network. */
bool cloud_restart_wanted(void);

/* Freshness check: a placed figure ends a running session;
 * the sync gesture and log-host command 6 make one pending. */
void cloud_sync_abort(void);

/* True only while a freshness check session runs (prompts
 * wait). */
bool cloud_sync_running(void);

/* True while figure events must wait (online round or synchronisation). */
bool cloud_holds_figures(void);
void cloud_sync_request(bool user);

/* A TLS connection to the configured server for the log stream; the
 * socket or a negative value. */
int16_t cloud_log_connect(uint32_t session);

/* The log connection opened by cloud_log_connect has been closed
 * (outcomes are posted only while none is open). */
void cloud_log_closed(void);

/* True while a request to the API host waits for a TLS socket and the log
 * connection is to give up its own. */
bool cloud_log_yield_wanted(void);

/* F12 command 73: schedules the update check once more when
 * the box is idle and no API request runs; 0 scheduled, 1 not. */
int cloud_remote_update_check(void);

void cloud_claim(const uint8_t uid[8], const uint8_t *token);

/* Setup page (page.c, which runs in this activity and lends
 * its buffers): certificate i (0 private.der, 1 client.der, 2 ca.der, the
 * CA file of the key ca) from the first size bytes of the work buffer,
 * checked (one DER element) and written: 0 the flash file
 * equals it, -1 refused, 1 the write failed. */
int cloud_cert_put(unsigned i, unsigned size);

/* Setup page: deletes host and port of /cfw/config.txt and its CA file
 * unless that is the vendor's /cert/ca.der (the ca= line stays); "ok" or
 * "card". */
const char *cloud_server_forget(void);

/* Setup page: a new text of /cfw/config.txt (terminated, changed in
 * place) checked as at start: NULL
 * when it may be stored (a valid server, or no host line: no server),
 * else the reason: "host", "vendor", "port". */
const char *cloud_config_check(char *text);

#endif
