/*
 * The player's part of the extension: settings in effect
 * (F6, F7, F10), event tags (F4, F11), the values set with the ears and
 * the sleep timer (F3). Called by the player activity only, apart from the
 * read-only state of player.h (ears, locks).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPLAY_H
#define XPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "ext.h"

/* At the start of the player activity: reads the global file. */
void xp_start(void);

/* About once per second and more often: re-reads the global file after a
 * settings file was stored and counts the sleep timer; true
 * when the timer has run out (playback stops as on removal). */
bool xp_poll(void);

/* A tag is placed (acted on): true when it is an event tag,
 * which has been handled (the box stays idle). Otherwise the
 * kept actions are applied, the figure's settings take
 * effect and its timer starts; xp_profile() then gives its profile. */
bool xp_place(const uint8_t uid[8]);
const ext_settings_t *xp_profile(void);

/* The tag on the box was removed (acted on). */
void xp_remove(void);

/* Remote pause: the sleep timer pauses as on removal, and
 * continues. */
void xp_timer_hold(bool hold);

/* A power-off: as a removal, and every sleep timer ends. */
void xp_power_off(void);

/* F9 is allowed: the global file holds no both-ears switch-off record 0;
 * read-only, for the input activity as well. */
bool xp_ears_off(void);

/* Ear requests while an event tag stands. */
void xp_ear_level(bool big);
void xp_ears_switch(void);

/* the record of a gesture stopped by a lock. */
void xp_locked(unsigned gesture, unsigned source);

#endif
