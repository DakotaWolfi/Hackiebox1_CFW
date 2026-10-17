/*
 * Extension protocol and storage: the hello and its answer
 * record, settings files received in the
 * freshness-check answer, the TBS files on the
 * card with the change report, the
 * state file and the shuffle order. The features
 * F1 to F12 that use them are the player's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXT_H
#define EXT_H

#include <stdbool.h>
#include <stdint.h>

/* the bits of the features this firmware implements, and no
 * other. A feature's bit is added here only together with the feature. */
#define EXT_CAP_F1_RESUME (1u << 0)
#define EXT_CAP_F2_SHUFFLE (1u << 1)
#define EXT_CAP_F3_SLEEP (1u << 2)
#define EXT_CAP_F4_EVENT_TAGS (1u << 3)
#define EXT_CAP_F5_RESTART (1u << 4)
#define EXT_CAP_F6_PER_FIGURE (1u << 5)
#define EXT_CAP_F7_GLOBAL (1u << 6)
#define EXT_CAP_F8_FOREIGN (1u << 7)
#define EXT_CAP_F10_LOCKS (1u << 8)
#define EXT_CAP_F11_EVENT_EXT (1u << 9) /* requires F4 */
#define EXT_CAP_F12_REMOTE (1u << 10)
#define EXT_CAP_F13_BATTERY (1u << 11)
/* 0xFFF. F9 has no bit. F12 has no bit dependency; its
 * commands 64 and 73 use ext_store() and cloud_remote_update_check()
 * (remote.c). */
#define EXT_CAPS (EXT_CAP_F1_RESUME | EXT_CAP_F2_SHUFFLE | EXT_CAP_F3_SLEEP | EXT_CAP_F4_EVENT_TAGS | EXT_CAP_F5_RESTART | \
                  EXT_CAP_F6_PER_FIGURE | EXT_CAP_F8_FOREIGN | EXT_CAP_F7_GLOBAL | EXT_CAP_F10_LOCKS |                     \
                  EXT_CAP_F11_EVENT_EXT | EXT_CAP_F12_REMOTE | EXT_CAP_F13_BATTERY)

/* --- protocol (called by the cloud activity) --- */

/* Fields 100 and 101 of a freshness-check answer that was accepted and
 * applied: msg is the whole answer body, already decoded
 * without error. */
void ext_answer(const uint8_t *msg, unsigned len);

/* F12 command 64: stores one field 100 (8 UID bytes and a
 * TBS file); true when the file was written. */
bool ext_store(const uint8_t *field, unsigned len);

/* The hello of this start has been received. */
bool ext_hello_received(void);
/* The hello answer of this start has been generated: from
 * then on the battery reports follow. */
bool ext_hello_answered(void);

/* Arguments 3 and 4 of the hello answer from the boot state
 * file; 0 until set. trial_pending: this start is a trial
 * start not yet confirmed, so the hello answer waits for the outcome. */
void ext_set_boot_result(uint32_t last_result, uint32_t rolled_back_build, bool trial_pending);

/* The trial start has been confirmed: last result 1. */
void ext_trial_confirmed(void);

/* Called by the cloud activity at least once a second: makes the hello
 * answer that waited for the trial start's confirmation. */
void ext_hello_flush(void);

/* Incremented whenever a settings file was stored or written: a reader
 * that caches values reads them again. */
uint32_t ext_generation(void);

/* --- settings files --- */

typedef enum
{
    EXT_PROFILE = 1,
    EXT_EVENT = 2,
    EXT_GLOBAL = 3
} ext_kind_t;

/* Record keys of the settings files. */
typedef enum
{
    EXT_RESUME = 0x01,
    EXT_SHUFFLE = 0x02,
    EXT_SLEEP = 0x03,         /* minutes, 0 off */
    EXT_SPEAKER_LIMIT = 0x04, /* step 0..15 */
    EXT_HEADPHONE_LIMIT = 0x05,
    EXT_LED = 0x06,       /* 0 on, 1 off, 2 dimmed */
    EXT_BOTH_EARS = 0x07, /* global only; absent = on */
    EXT_SLAP_LOCK = 0x08,
    EXT_TILT_LOCK = 0x09,
    EXT_VOLUME_LOCK = 0x0A,
} ext_key_t;

/* A profile or the global file: value[k] of key k counts when bit k of
 * present is set. */
typedef struct
{
    uint8_t kind;
    uint16_t present;
    uint16_t value[11];
    uint32_t stamp;
} ext_settings_t;

static inline bool ext_has(const ext_settings_t *s, ext_key_t k)
{
    return s->present & (1u << k);
}

static inline void ext_set(ext_settings_t *s, ext_key_t k, uint16_t v)
{
    s->present |= (uint16_t)(1u << k);
    s->value[k] = v;
}

static inline void ext_clear(ext_settings_t *s, ext_key_t k)
{
    s->present &= (uint16_t) ~(1u << k);
    s->value[k] = 0;
}

/* An event tag file: the first 16 action records in their
 * order (any action number; the player gives 0 and 17 to 255 no effect). */
typedef enum
{
    EXT_TOGGLE = 0,
    EXT_ON = 1,
    EXT_OFF = 2,
    EXT_CLEAR = 3
} ext_mode_t;
typedef enum
{
    EXT_SCOPE_NONE,
    EXT_SCOPE_FIGURE,
    EXT_SCOPE_GLOBAL
} ext_scope_t;

typedef struct
{
    uint8_t n;
    bool tones; /* record 0x11, absent = on */
    struct
    {
        uint8_t action;
        uint8_t mode;   /* ext_mode_t */
        uint16_t value; /* bits 0-13 of the parameter */
    } a[16];
    uint32_t stamp;
} ext_event_t;

/* The scope of an event tag: that of its first action 1 to 16. */
ext_scope_t ext_event_scope(const ext_event_t *e);

/* The tag's profile / event tag file, the global file; false when absent
 * (missing, unreadable or invalid). Each reads the card. */
bool ext_profile(const uint8_t uid[8], ext_settings_t *out);
bool ext_event_tag(const uint8_t uid[8], ext_event_t *out);
bool ext_global(ext_settings_t *out);

/* writes a profile (uid) or the global file (uid NULL) with
 * the settings of s, origin 0 and the time stamp of the clock (stored back
 * in s), and reports it after the hello. */
bool ext_write(const uint8_t uid[8], ext_settings_t *s);

/* the checks of a whole file of n bytes. */
bool ext_tbs_valid(const uint8_t *f, unsigned n);

/* "CFW/EXT/<D>/<F>.TBS" (needs 30 bytes), or "CFW/EXT/GLOBAL.TBS" for
 * uid NULL or all zero. */
void ext_path(const uint8_t uid[8], char *path);

/* Parses a valid file of kind 1 or 3 (records of other kinds skipped). */
void ext_parse_settings(const uint8_t *f, ext_settings_t *out);
void ext_parse_event(const uint8_t *f, ext_event_t *out);

/* Builds a settings file into out (at most 64 bytes); its size. */
unsigned ext_build(uint8_t kind, uint32_t stamp, const ext_settings_t *s, uint8_t *out);

/* The packed arguments 3 and 4 of the settings report. */
void ext_pack_report(const ext_settings_t *s, uint32_t *packed, uint32_t *values);

/* --- state file --- */

typedef struct
{
    uint8_t uid[8];
    uint32_t sequence; /* 0: free */
    uint32_t audio_id;
    uint32_t position; /* next block */
    uint32_t seed;
    uint8_t index, chapters;
    uint8_t flags; /* EXT_STATE_SHUFFLING */
} ext_state_t;
#define EXT_STATE_SHUFFLING 0x01

void ext_state_pack(const ext_state_t *s, uint8_t out[32]);
/* false when the record is free (sequence 0 or check wrong). */
bool ext_state_unpack(const uint8_t in[32], ext_state_t *s);

/* The record of uid in /ext/STATE.BIN; false when there is none. */
bool ext_state_get(const uint8_t uid[8], ext_state_t *out);
/* Writes s (its sequence is set to one above the highest in the file)
 * into the record of its UID, else the first free, else the oldest. */
bool ext_state_put(ext_state_t *s);

/* --- shuffle order --- */

/* order[k] = chapter at shuffle index k, for 2 <= n <= 99. */
void ext_shuffle(uint32_t seed, unsigned n, uint8_t *order);

#endif
