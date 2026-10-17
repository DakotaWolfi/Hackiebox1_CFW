/*
 * Tones of the beep generator and the chime catalogue. Called by the sound activity only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TONES_H
#define TONES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    CHIME_SIGNAL = 0,
    CHIME_UNUSED_1 = 1,
    CHIME_CHAPTER_BACK = 2,
    CHIME_CHAPTER_FORWARD = 3,
    CHIME_SILENT = 4,
    CHIME_WIND_BACK = 5,
    CHIME_WIND_FORWARD = 6,
    CHIME_DOWNLOAD_COMPLETE = 7,
    CHIME_CHARGING_STATION = 8,
    CHIME_VOLUME_DOWN = 9,
    CHIME_VOLUME_MIN = 10,
    CHIME_VOLUME_UP = 11,
    CHIME_VOLUME_LIMIT = 12,
    CHIME_COUNT
} chime_t;

/* Gain in dB of a step on the current path's curve; set by
 * the sound activity before each chime. */
typedef int (*tones_gain_fn)(int step);

/* Called at least every 20 ms while a chime waits or fades: the sound
 * activity's muting and unmuting; may be NULL. */
typedef void (*tones_poll_fn)(void);

/* Plays a chime at the given volume step and returns when its last tone
 * has started (fading tones: when the fade has ended); returns how late
 * the latest note started, in ms. */
uint32_t tones_play_chime(chime_t c, int step, tones_gain_fn gain, tones_poll_fn poll);

/* The feedback tone pairs for n results (bit i set: "on"),
 * at the normal tone level of step. */
void tones_play_feedback(uint16_t results, unsigned n, int step, tones_gain_fn gain, tones_poll_fn poll);

/* Plays one tone of frequency f (one of the tone table) and
 * duration d ms at volume code a, respecting the end time of the previous
 * tone. Returns false for an unknown frequency. */
bool tones_play(unsigned f, unsigned d, unsigned a);

#endif
