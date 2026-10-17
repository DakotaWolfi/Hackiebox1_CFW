/*
 * LED requests and patterns.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LEDUI_H
#define LEDUI_H

#include <stdbool.h>

typedef enum
{
    /* continuous */
    LED_OFF,
    LED_READY_IDLE,
    LED_READY_PLAYING,
    LED_SYNC,
    LED_FIRMWARE_DOWNLOAD,
    LED_CONTENT_DOWNLOAD,
    LED_POWER_ACTION,
    LED_CONNECTING,
    LED_SEARCH,
    LED_SETUP,
    LED_NETWORK_ERROR,
    LED_NEW_IMAGE,
    /* one-shot */
    LED_CONFIRMATION,
    LED_ERROR,
    LED_STATION_REMOVED,
    LED_STATION_REMOVED_LOW,
    LED_BATTERY_EMPTY,
    LED_BATTERY_LOW,
    /* one-shot, extension: 5000 ms each */
    LED_EXT_BLINK,    /* one turquoise blink, then dark */
    LED_EXT_LILAC,    /* one lilac blink, then dark */
    LED_EXT_ON,       /* five turquoise blinks */
    LED_EXT_OFF,      /* five lilac blinks */
    LED_EXT_MIXED,    /* five blinks, turquoise and lilac alternating */
    LED_EXT_LEVEL_ON, /* level display */
    LED_EXT_LEVEL_DIMMED,
    LED_EXT_LEVEL_OFF,
    /* others */
    LED_COLOUR_CHANGE,
    LED_ACTIVITY,
} led_request_t;

void ledui_start(void);
void ledui_request(led_request_t r);
/* Sets or clears the low-battery state's orange ready colour. */
void ledui_low_battery(bool on);
/* Sets or clears offline mode's white ready colour; orange
 * has priority. */
void ledui_offline(bool on);
/* LED level of the extension replacing the server's setting (0 on, 1 off,
 * 2 dimmed; -1 none), shown at once. */
void ledui_ext_level(int level);

#endif
