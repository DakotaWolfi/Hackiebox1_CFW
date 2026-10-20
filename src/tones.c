/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "codec.h"
#include "debug.h"
#include "tones.h"

typedef struct
{
    uint16_t f, s, c;
} coeff_t;

/* Sine and cosine coefficients for 16 kHz (exact values). */
static const coeff_t coeffs[] = {
    {300, 3850, 32538},
    {440, 5629, 32279},
    {500, 6389, 32135},
    {523, 6681, 32079},
    {554, 7071, 31997},
    {587, 7484, 31902},
    {659, 8382, 31676},
    {740, 9384, 31395},
    {784, 9928, 31227},
    {800, 10122, 31165},
    {831, 10502, 31037},
    {880, 11095, 30831},
    {932, 11721, 30595},
    {1000, 12537, 30271},
};

typedef struct
{
    uint16_t start, f, d;
    uint8_t quiet, fading;
} note_t;

#define N(start, f, d) {start, f, d, 0, 0}
#define Q(start, f, d) {start, f, d, 1, 0}
#define F(start, f, d) {start, f, d, 0, 1}

static const note_t chime0[] = {N(0, 880, 70), F(180, 440, 1200)};
static const note_t chime1[] = {N(0, 831, 130), Q(250, 587, 130), N(500, 784, 130),
                                Q(750, 554, 130), N(1000, 740, 130), Q(1250, 523, 130)};
static const note_t chime2[] = {N(0, 587, 85), N(155, 440, 85)};
static const note_t chime3[] = {N(0, 587, 85), N(155, 784, 85)};
static const note_t chime5[] = {Q(0, 932, 100), Q(100, 880, 100), Q(200, 831, 100), Q(300, 784, 100)};
static const note_t chime6[] = {Q(0, 784, 100), Q(100, 831, 100), Q(200, 880, 100), Q(300, 932, 100)};
static const note_t chime7[] = {N(0, 659, 40), F(150, 523, 1050)};
static const note_t chime8[] = {N(0, 587, 95), F(100, 880, 1470)};
static const note_t chime9[] = {N(0, 500, 20)};
static const note_t chime10[] = {N(0, 300, 20), N(50, 300, 20)};
static const note_t chime11[] = {N(0, 800, 20)};
static const note_t chime12[] = {N(0, 1000, 20), N(50, 1000, 20)};

static const struct
{
    const note_t *notes;
    uint8_t n;
} catalogue[CHIME_COUNT] = {
#define E(a) {a, sizeof a / sizeof a[0]}
    [0] = E(chime0),
    [1] = E(chime1),
    [2] = E(chime2),
    [3] = E(chime3),
    [4] = {0, 0},
    [5] = E(chime5),
    [6] = E(chime6),
    [7] = E(chime7),
    [8] = E(chime8),
    [9] = E(chime9),
    [10] = E(chime10),
    [11] = E(chime11),
    [12] = E(chime12),
#undef E
};

static TickType_t end_time; /* end of the last tone */
static bool any_tone;
static tones_poll_fn poll; /* of the chime being played */

/* Waits until tick at, calling poll at least every 20 ms (muting and
 * unmuting go on while a chime plays). */
static void wait_until(TickType_t at)
{
    for (;;)
    {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(at - now) <= 0)
        {
            return;
        }
        vTaskDelay(at - now > 20 ? 20 : at - now);
        if (poll)
        {
            poll();
        }
    }
}

/* Volume code for the gain g in dB. */
static unsigned volume_code(int g)
{
    if (g > 0)
    {
        return 0;
    }
    if (g < -43)
    {
        return 45;
    }
    return (unsigned)(2 - g);
}

static void wait_for_previous(void)
{
    if (!any_tone)
    {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(end_time - now) <= 0)
    {
        return;
    }
    uint32_t r = end_time - now;
    uint32_t n = (r + 19) / 20;
    uint32_t w = n * r - 10 * n * (n - 1);
    wait_until(now + w);
}

/* The end time counts in whole ticks from after the start was written, so
 * the next tone can be due while the generator still plays the last
 * samples of the previous one; a start written then is lost and the
 * chime has a gap. Waits up to 20 ms for bit 7 of register 71 to clear
 * (the read allowed during playback). Only then is it a bus overrun,
 * reported here; its bus recovery is not done yet. */
static void wait_generator_idle(void)
{
    if (!any_tone)
    {
        return;
    }
    for (unsigned ms = 0; ms <= 20; ms++)
    {
        uint8_t v;
        if (!codec_read(71, &v) || !(v & 0x80))
        {
            if (ms > 1)
            {
                debug_printf("snd: beeper busy %u ms\n", ms);
            }
            return;
        }
        vTaskDelay(1);
    }
    debug_printf("snd: beeper busy at next tone (6305)\n");
}

bool tones_play(unsigned f, unsigned d, unsigned a)
{
    const coeff_t *c = 0;

    for (unsigned i = 0; i < sizeof coeffs / sizeof coeffs[0]; i++)
    {
        if (coeffs[i].f == f)
        {
            c = &coeffs[i];
        }
    }
    if (!c)
    {
        return false;
    }

    wait_for_previous();
    wait_generator_idle();
    uint32_t samples = 16u * d;
    codec_write(CODEC_REG(0, 73), (uint8_t)(samples >> 16));
    codec_write(CODEC_REG(0, 74), (uint8_t)(samples >> 8));
    codec_write(CODEC_REG(0, 75), (uint8_t)samples);
    codec_write(CODEC_REG(0, 76), (uint8_t)(c->s >> 8));
    codec_write(CODEC_REG(0, 77), (uint8_t)c->s);
    codec_write(CODEC_REG(0, 78), (uint8_t)(c->c >> 8));
    codec_write(CODEC_REG(0, 79), (uint8_t)c->c);
    codec_write(CODEC_REG(0, 72), 0x80);
    codec_write(CODEC_REG(0, 71), (uint8_t)(0x80 + a));
    end_time = xTaskGetTickCount() + d;
    any_tone = true;
    return true;
}

/* Rewrites the volume code of a running tone so that its level falls by
 * 1 dB per interval. */
static void fade(unsigned a0, unsigned d)
{
    TickType_t start = xTaskGetTickCount();
    unsigned interval = a0 < 63 ? d / (63 - a0) : d;
    TickType_t next = start;

    for (unsigned a = a0;; a++)
    {
        if (xTaskGetTickCount() - start >= d)
        {
            return;
        }
        codec_write(CODEC_REG(0, 71), (uint8_t)(0x80 + a));
        next += interval;
        wait_until(next);
        if (a >= 63)
        {
            return;
        }
    }
}

uint32_t tones_play_chime(chime_t ch, int step, tones_gain_fn gain, tones_poll_fn p)
{
    if (ch >= CHIME_COUNT || catalogue[ch].n == 0)
    {
        return 0;
    }
    poll = p;
    TickType_t t0 = xTaskGetTickCount();
    uint32_t late = 0;
    for (unsigned i = 0; i < catalogue[ch].n; i++)
    {
        const note_t *n = &catalogue[ch].notes[i];
        wait_until(t0 + n->start);
        int s = step - (n->quiet ? 3 : 0);
        unsigned a = volume_code(gain(s < 0 ? 0 : s));
        tones_play(n->f, n->d, a);
        /* the note started this much after its time (codec writes on a
         * busy I2C bus, a busy CPU) */
        TickType_t l = xTaskGetTickCount() - (t0 + n->start);
        if (l > late)
        {
            late = l;
        }
        if (n->fading)
        {
            fade(a, n->d);
        }
    }
    poll = 0;
    return late;
}

/* per result a pair of normal tones, 659 Hz then 880 Hz 150 ms
 * later ("on") or the reverse ("off"), 100 ms each; pairs 400 ms apart. */
void tones_play_feedback(uint16_t results, unsigned n, int step, tones_gain_fn gain, tones_poll_fn p)
{
    poll = p;
    unsigned a = volume_code(gain(step));
    TickType_t t0 = xTaskGetTickCount();
    for (unsigned i = 0; i < n; i++)
    {
        bool on = results & (1u << i);
        wait_until(t0 + 400 * i);
        tones_play(on ? 659 : 880, 100, a);
        wait_until(t0 + 400 * i + 150);
        tones_play(on ? 880 : 659, 100, a);
    }
    poll = 0;
}
