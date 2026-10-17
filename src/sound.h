/*
 * Sound activity: owns the codec. Configures it at start, selects the
 * output path from the headset state, applies volume steps and plays the
 * chimes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>
#include "tones.h"

/* Creates the request queue; before the scheduler starts, so that requests
 * can be posted before the codec is configured. */
void sound_init(void);
/* Starts the sound activity, which configures the codec. */
void sound_start(void);

/* Requests from other activities; handled in order. */
void sound_ear_big(void);
void sound_ear_small(void);
void sound_chime(chime_t c);
/* Timed pass and accelerometer events read the headset state. */
void sound_check_headset(void);
/* The same, waiting until the read (and a path switch it causes) is done,
 * at most max_ms; false on timeout (a chime's fade delays it up to about
 * 1.6 s). For the input activity before it evaluates a slap. */
bool sound_check_headset_sync(uint32_t max_ms);
/* the stream output has ended (mute) or a mute is wanted;
 * the DAC follows the latest wanted state, also when requests are lost. */
void sound_stream(bool playing);
void sound_stream_from_isr(bool playing);
/* Unmutes before the stream output starts and waits until the gain of the
 * step has been set, at most max_ms; false on timeout. */
bool sound_stream_start(uint32_t max_ms);

/* mute, switch to the parked path and stop all handling for
 * the rest of the run; waits at most max_ms (500 for the shutdown, chapter
 * PWR) and returns true when done. Not for the restart gesture or a fatal
 * error. */
bool sound_park(uint32_t max_ms);

/* The volume limits in settings changed. */
void sound_limits_changed(void);

/* --- extension --- */

/* Limits replacing the server limits, step 0..15 per output; 0xFF: the
 * server limit. */
void sound_ext_limits(uint8_t speaker, uint8_t headphones);
/* Sets the step of a path (1 speaker, 2 headphones, 0 the current one),
 * lowered to its limit, without a chime. */
void sound_set_step(unsigned path, unsigned step);
/* Sleep-timer fade: the current path's step becomes
 * floor(s / 16) and the gain is set between steps in 0.5 dB units. */
void sound_fade(unsigned sixteenths);
/* Feedback tone pairs: bit i of results is the result of
 * action i ("on" rising, "off" falling), n pairs 400 ms apart. */
void sound_feedback(uint16_t results, unsigned n);
/* The selected path (0 parked, 1 speaker, 2 headphones), a path's step,
 * and the number of ear steps made so far. */
unsigned sound_path(void);
unsigned sound_step(unsigned path);
unsigned sound_limit(unsigned path); /* the limit in effect */
uint32_t sound_ear_steps(void);

/* Millisecond counter at the end of the last path switch for a headphone
 * change, 0 when the headphone state has not changed. */
uint32_t sound_headphone_ref(void);

#endif
