/*
 * Retained state of the power handling and its
 * encoding in the two words of the retained record.
 *
 * Word 1: bits 0-25 resume record (12-bit tag, half block
 * number << 12), 26-27 speaker limit, 28-29 headphone limit, 30 slap
 * enable, 31 slap direction 1. Word 2: bits 0-3 speaker step, 4-7
 * headphone step, 8-9 LED brightness, 10 offline mode, 11 on the station,
 * 12 freshness check field 6, 29 always 1 (a saved state is never all
 * zero), 30 always 0 (not a fault record), 31 saved by the off state
 * without hibernation (CFW_HIBERNATE 0: restored after the MCU reset).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PWRSTATE_H
#define PWRSTATE_H

#include <stdbool.h>
#include <stdint.h>

#define PWRSTATE_SOFT_OFF (1u << 31) /* word 2 */

typedef struct
{
    uint8_t limit_speaker, limit_headphones; /* 0..3 */
    uint8_t step_speaker, step_headphones;   /* 0..15 */
    bool slap_enable, slap_direction_1, field6;
    uint8_t led; /* 0..3 */
    bool offline, on_station;
    uint32_t resume; /* 26 bits, 0: nothing */
} pwrstate_t;

void pwrstate_encode(const pwrstate_t *s, bool soft_off, uint32_t w[2]);
/* False when the words are no state record of pwrstate_encode (all zero,
 * a fault record, bits 13-28 of word 2 not zero or bit 29 not set). */
bool pwrstate_decode(const uint32_t w[2], pwrstate_t *s);
/* the 26-bit resume value of a UID (B0 first) and a block. */
uint32_t pwrstate_resume(const uint8_t uid[8], uint32_t block);

#endif
