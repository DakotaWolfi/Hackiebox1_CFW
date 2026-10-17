/*
 * Log records to the log host: function and event records,
 * the record buffer, the log connection and the log-host commands.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RTNL_H
#define RTNL_H

#include <stdbool.h>
#include <stdint.h>

enum
{
    LOG_INFO = 1,
    LOG_WARN = 3,
    LOG_ERROR = 4
};

/* Event types. */
typedef enum
{
    EVT_BIG_EAR = 1,
    EVT_SMALL_EAR = 2,
    EVT_SLAP_FORWARD = 3,
    EVT_SLAP_BACKWARD = 4,
    EVT_TILT_FORWARD = 5,
    EVT_TILT_BACKWARD = 6,
    EVT_CHARGER_ON = 7,
    EVT_CHARGER_OFF = 8,
    EVT_HEADPHONES_IN = 9,
    EVT_HEADPHONES_OUT = 10,
    EVT_PLAYBACK_STARTING = 11,
    EVT_PLAYBACK_STARTED = 12,
    EVT_PLAYBACK_STOPPED = 13,
} log_event_t;

/* Argument payload builder. */
typedef struct
{
    uint8_t n;
    uint8_t data[80];
} log_args_t;

void log_arg_num(log_args_t *a, int32_t v);
void log_arg_text(log_args_t *a, const char *s);

/* A function record; variant 0 when the row has none (field 7 absent). */
void log_fn(uint8_t level, uint16_t group, uint16_t function, uint8_t variant, const log_args_t *a);

/* An event record; uid in air order or NULL. */
void log_event(log_event_t type, const uint8_t *uid);

/* The cloud has finished a round of API requests in session s. */
void log_round_done(uint32_t s);

/* True while a log connection is open. */
bool log_connected(void);

/* Log drain (up to 5.52 s); does nothing
 * without a connection, while a Wi-Fi change is pending, in the log task
 * itself and in an interrupt handler. */
void log_drain(void);

/* log_init before the activities start; log_start once the MAC address
 * is known (its last byte sets the pause of command 2). */
void log_init(void);
void log_start(uint8_t mac_last_byte);

#endif
