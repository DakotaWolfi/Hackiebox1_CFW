/*
 * Playback control and the playing
 * of content files: one activity that owns the decoder and
 * acts on figure and gesture events in its order of arrival.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>

/* player_init before the activities start (others post events early). */
void player_init(void);
void player_start(void);

/* Figure events; uid in air order, B0 first. */
/* token: the 32 data bytes of the tag (auth token), NULL when none. */
void player_placed(const uint8_t uid[8], const uint8_t *token);
void player_removed(void);

/* Gesture requests: slap = chapter, tilt = wind. */
void player_chapter(bool forward);
void player_wind(bool forward);

/* True while a figure's content plays (state Content). */
bool player_playing(void);

/* True while a figure is being handled (content prepared or playing). */
bool player_busy(void);
/* Milliseconds spent in the Opus decoder, for the download rate
 * diagnostics (cloud.c resets it). */
extern uint32_t player_decode_ms;

/* Sound prompt nn; wait: 1000 ms before it. */
void player_prompt(unsigned nn, bool wait);

/* Sync gesture and the end of a session. */
void player_sync_request(void);
void player_sync_result(int error, bool user);

/* --- extension, for the input activity --- */

/* What the ears do while an event tag stands:
 * EARS_VOLUME: the volume, without the volume lock; EARS_LED: a short press
 * goes to player_ear_level instead of the volume. */
typedef enum
{
    EARS_NORMAL,
    EARS_VOLUME,
    EARS_LED
} player_ears_t;
player_ears_t player_ears(void);
/* A combined tag: one ear held 3 s goes to player_ears_switch, not to a
 * sync request. */
bool player_ears_combined(void);
void player_ear_level(bool big);
void player_ears_switch(void);

/* The locks in effect: -1 no record (the original behaviour applies,
 * slap enable included), 0 unlocked, 1 locked; source 1 the figure's
 * profile, 2 the global file. */
typedef enum
{
    LOCK_SLAP,
    LOCK_TILT,
    LOCK_VOLUME
} player_lock_t;
int player_lock(player_lock_t l, unsigned *source);
/* gesture 1 slap forward, 2 slap back, 3 tilt forward, 4 tilt
 * back, 5 big ear, 6 small ear, 7 the F5 gesture. */
void player_locked(unsigned gesture, unsigned source);

/* a slap of the upside-down box at tick at. */
void player_restart_gesture(uint32_t at);

/* F9: a short both-ears hold. Unless the global file forbids
 * it or the box is shutting down: one lilac blink, playback stops as on
 * removal (the sleep timer ends) and the idle power-off follows. */
void player_switch_off(void);

/* --- remote control, for the log activity ---
 * Each call decides its result at once from the player's state and posts
 * the work to the player activity (remote command result codes). The log
 * activity counts each command except 72 as activity for the idle
 * power-off (power_activity()). */
enum
{
    PLAYER_RC_DONE = 0,
    PLAYER_RC_STATE = 1,
    PLAYER_RC_ARGS = 2
};
int player_remote_pause(void);                /* 65: while content plays */
int player_remote_continue(void);             /* 66: while paused */
int player_remote_stop(void);                 /* 67: while content plays or is paused */
int player_remote_chapter(bool forward);      /* 68, 69: as a slap, with its chime */
int player_remote_volume(unsigned step);      /* 70: 1 when the limit capped it */
int player_remote_play(const uint8_t uid[8]); /* 71: no figure on the box, idle */

/* The player status without the battery. The log activity polls it
 * about once per second and sends the record when state, uid, chapter or
 * volume differ from the last one sent. */
typedef struct
{
    uint8_t state;       /* 0 idle without a figure, 1 idle with one, 2 playing, 3 paused, 4 preparing */
    uint8_t uid[8];      /* B0 ... B7, zero without one */
    uint32_t audio_id;   /* while playing or paused, else 0 */
    uint32_t chapter;    /* from 0, the chapter in the file */
    uint32_t position;   /* next block to read */
    uint8_t step, limit; /* current output */
    uint8_t headphones;  /* 1 headphones, 0 speaker */
    uint8_t locks;       /* bit 0 slap, 1 tilt, 2 volume */
} player_status_t;
void player_status(player_status_t *s);

#endif
