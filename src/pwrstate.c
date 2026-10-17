/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pwrstate.h"

void pwrstate_encode(const pwrstate_t *s, bool soft_off, uint32_t w[2])
{
    w[0] = (s->resume & 0x3FFFFFFu) | (uint32_t)(s->limit_speaker & 3) << 26 |
           (uint32_t)(s->limit_headphones & 3) << 28 | (uint32_t)s->slap_enable << 30 |
           (uint32_t)s->slap_direction_1 << 31;
    w[1] = (s->step_speaker & 15u) | (s->step_headphones & 15u) << 4 | (s->led & 3u) << 8 |
           (uint32_t)s->offline << 10 | (uint32_t)s->on_station << 11 | (uint32_t)s->field6 << 12 | 1u << 29 |
           (soft_off ? PWRSTATE_SOFT_OFF : 0);
}

bool pwrstate_decode(const uint32_t w[2], pwrstate_t *s)
{
    /* Bits 13-30 of word 2 as pwrstate_encode leaves them (only bit 29):
     * this also refuses all zero and a fault record, and a record of
     * another firmware or garbage after a reset is not restored. */
    if ((w[1] & 0x7FFFE000u) != 1u << 29)
    {
        return false;
    }
    s->resume = w[0] & 0x3FFFFFFu;
    s->limit_speaker = w[0] >> 26 & 3;
    s->limit_headphones = w[0] >> 28 & 3;
    s->slap_enable = w[0] >> 30 & 1;
    s->slap_direction_1 = w[0] >> 31;
    s->step_speaker = w[1] & 15;
    s->step_headphones = w[1] >> 4 & 15;
    s->led = w[1] >> 8 & 3;
    s->offline = w[1] >> 10 & 1;
    s->on_station = w[1] >> 11 & 1;
    s->field6 = w[1] >> 12 & 1;
    return true;
}

uint32_t pwrstate_resume(const uint8_t uid[8], uint32_t block)
{
    uint32_t half = block / 2;
    if (half > 16383)
    {
        half = 16383;
    }
    if (!half)
    {
        return 0; /* nothing to resume */
    }
    return (uid[0] | (uint32_t)(uid[1] & 15) << 8) | half << 12;
}
