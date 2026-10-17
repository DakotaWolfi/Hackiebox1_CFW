/*
 * Audio serial port: McASP as I2S clock master to the codec, stream data by
 * uDMA ping-pong transfers. The clocks run
 * continuously (the codec's tones need them); stream output starts with a
 * restart of the port and the unmute, and ends when the data runs out, with
 * the mute. No gap is filled with generated samples.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdbool.h>
#include <stdint.h>

/* Frames (stereo sample pairs) per DMA half: 16 ms at 16 kHz. */
#define AUDIO_OUT_FRAMES 256

/* First start of the port after the codec configuration: clocks only. */
void audio_out_start(void);

/* True once the audio serial port has been started. */
bool audio_out_running(void);

/* Diagnostic line: interrupt count, ring counters, output state. */
void audio_out_report(void);

/* Stream data: a free buffer of AUDIO_OUT_FRAMES interleaved stereo frames
 * (waits while all are queued, NULL when the port took none for 1 s),
 * then hand it over. Output starts once all buffers are waiting, after the
 * DAC has been unmuted. */
int16_t *audio_out_buffer(void);
void audio_out_commit(void);
/* The same for a buffer holding only frames frames (the last of a
 * stream); no samples are added. */
void audio_out_commit_frames(unsigned frames);

/* The producer's stream has ended: what is queued plays out (output starts
 * for it if needed); running out of buffers from now on is not an
 * underrun. */
void audio_out_end(void);

/* Stops the output at once and discards the queued audio. */
void audio_out_flush(void);

/* Waits until the queued audio has played out, at most ms; true when it
 * has. */
bool audio_out_wait_idle(uint32_t ms);

/* Number of outputs that ended because the data ran out mid-stream. */
uint32_t audio_out_underruns(void);

#endif
