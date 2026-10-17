/*
 * Settings from the freshness check and the language number. Kept in memory; their
 * storage across hibernation and in the settings file follows.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t limit_speaker; /* 0..3 (steps 6, 9, 12, 15) */
    uint8_t limit_headphones;
    uint8_t slap_enable;    /* 1 on */
    uint8_t slap_direction; /* 1: direction A is forward */
    uint8_t reserved6;
    uint8_t led; /* 0 on, 1 off, 2 dimmed */
} settings_t;

extern settings_t settings;

/* Reads /settings.cfg and the first header of /fw/sfx.bin from the flash;
 * after the network
 * processor has started. */
void settings_load(void);

/* True once settings_load has run: the network processor's start-up has
 * ended and the flash file system can be used. */
bool settings_ready(void);

/* Language number of the box. */
uint8_t settings_language(void);

/* Stores a new language index 0..3 in /settings.cfg,
 * keeping country code and factory language index. */
void settings_set_language(uint8_t l);

/* the two-character country code of /settings.cfg (a shorter
 * code padded with 0); false when the file holds none. */
bool settings_country(char cc[2]);

/* The settings message for language index l (fields 1, 5, 6). */
unsigned settings_encode(uint8_t l, uint8_t *out);

/* Audio id of the first archive entry, 1 when not usable. */
uint32_t settings_archive_audio_id(void);

/* Volume limit step of a value 0..3. */
static inline uint8_t settings_limit_step(uint8_t v)
{
    return (uint8_t)(6 + 3 * v);
}

#endif
