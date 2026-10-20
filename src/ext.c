/*
 * Extension protocol and storage.
 * Nothing is cached: the files are read from the card when asked for.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "clock.h"
#include "crc32.h"
#include "debug.h"
#include "ext.h"
#include "rtnl.h"
#include "sdfiles.h"
#include "version.h"

#define TBS_MAX 256
#define STATE_PATH "CFW/EXT/STATE.BIN"
#define STATE_RECORDS 64

static volatile bool hello; /* not kept across a start */
static volatile bool answered;
static volatile uint32_t boot_result;
static uint32_t boot_rolled_back;
/* A hello in an unconfirmed trial start is answered once the trial is
 * confirmed, so that the server learns the result in this start, not in
 * the next one. */
static volatile bool trial_waiting, trial_confirmed, answer_due;
static volatile uint32_t generation;

static uint32_t ext_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ext_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
    {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

bool ext_hello_received(void)
{
    return hello;
}

bool ext_hello_answered(void)
{
    return answered;
}

void ext_set_boot_result(uint32_t last_result, uint32_t rolled_back_build, bool trial_pending)
{
    if (!trial_confirmed)
    {
        boot_result = last_result;
    }
    boot_rolled_back = rolled_back_build;
    trial_waiting = trial_pending && !trial_confirmed;
}

/* From the activity that confirms (op_task): the result first, then the
 * flag the cloud activity reads. */
void ext_trial_confirmed(void)
{
    boot_result = 1;
    trial_confirmed = true;
    trial_waiting = false;
}

static void hello_answer(void);

void ext_hello_flush(void)
{
    if (answer_due && !trial_waiting)
    {
        answer_due = false;
        hello_answer();
    }
}

uint32_t ext_generation(void)
{
    return generation;
}

/* --- TBS files --- */

void ext_path(const uint8_t uid[8], char *path)
{
    static const uint8_t zero[8];
    if (!uid || !memcmp(uid, zero, 8))
    {
        strcpy(path, "CFW/EXT/GLOBAL.TBS");
        return;
    }
    /* upper-case digits; FatFs matches without regard to case */
    snprintf(path, 30, "CFW/EXT/%02X%02X%02X%02X/%02X%02X%02X%02X.TBS", uid[0], uid[1], uid[2], uid[3], uid[4],
             uid[5], uid[6], uid[7]);
}

/* length, highest value and the kinds (bit kind)
 * of the keys 0x01 to 0x0A. */
static const struct
{
    uint8_t len;
    uint8_t kinds;
    uint16_t max;
} keys[11] = {
    [EXT_RESUME] = {1, 1u << EXT_PROFILE, 1},
    [EXT_SHUFFLE] = {1, 1u << EXT_PROFILE, 1},
    [EXT_SLEEP] = {2, 1u << EXT_PROFILE, 65535},
    [EXT_SPEAKER_LIMIT] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 15},
    [EXT_HEADPHONE_LIMIT] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 15},
    [EXT_LED] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 2},
    [EXT_BOTH_EARS] = {1, 1u << EXT_GLOBAL, 1},
    [EXT_SLAP_LOCK] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 1},
    [EXT_TILT_LOCK] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 1},
    [EXT_VOLUME_LOCK] = {1, 1u << EXT_PROFILE | 1u << EXT_GLOBAL, 1},
};

bool ext_tbs_valid(const uint8_t *f, unsigned n)
{
    if (n < 16 || n > TBS_MAX || memcmp(f, "TBS1", 4) || f[4] < EXT_PROFILE || f[4] > EXT_GLOBAL)
    {
        return false;
    }
    unsigned body = (unsigned)f[6] | (unsigned)f[7] << 8;
    if (n != 16 + body || crc32_ogg(0, f, 12 + body) != ext_le32(f + 12 + body))
    {
        return false;
    }
    /* the records fill the body exactly */
    for (unsigned p = 0; p < body; p += 2u + f[12 + p + 1])
    {
        if (body - p < 2 || f[12 + p + 1] > body - p - 2)
        {
            return false;
        }
    }
    return true;
}

void ext_parse_settings(const uint8_t *f, ext_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->kind = f[4];
    s->stamp = ext_le32(f + 8);
    unsigned body = (unsigned)f[6] | (unsigned)f[7] << 8;
    for (unsigned p = 0; p < body; p += 2u + f[12 + p + 1])
    {
        const uint8_t *r = f + 12 + p;
        unsigned k = r[0];
        /* unknown, not allowed in this kind, other length, out of range:
         * skipped; the last valid one counts */
        if (k < 1 || k > 10 || !(keys[k].kinds & (1u << s->kind)) || r[1] != keys[k].len)
        {
            continue;
        }
        uint16_t v = r[1] == 2 ? (uint16_t)(r[2] | r[3] << 8) : r[2];
        if (v > keys[k].max)
        {
            continue;
        }
        ext_set(s, (ext_key_t)k, v);
    }
}

void ext_parse_event(const uint8_t *f, ext_event_t *e)
{
    memset(e, 0, sizeof *e);
    e->tones = true;
    e->stamp = ext_le32(f + 8);
    unsigned body = (unsigned)f[6] | (unsigned)f[7] << 8;
    for (unsigned p = 0; p < body; p += 2u + f[12 + p + 1])
    {
        const uint8_t *r = f + 12 + p;
        if (r[0] == 0x10 && r[1] == 3 && e->n < 16)
        {
            uint16_t par = (uint16_t)(r[3] | r[4] << 8);
            e->a[e->n].action = r[2];
            e->a[e->n].mode = (uint8_t)(par >> 14);
            e->a[e->n].value = par & 0x3FFF;
            e->n++;
        }
        else if (r[0] == 0x11 && r[1] == 1 && r[2] <= 1)
        {
            e->tones = r[2];
        }
    }
}

ext_scope_t ext_event_scope(const ext_event_t *e)
{
    for (unsigned i = 0; i < e->n; i++)
    {
        uint8_t a = e->a[i].action;
        if (a == 8 || a == 9 || (a >= 13 && a <= 16))
        {
            return EXT_SCOPE_GLOBAL;
        }
        if (a >= 1 && a <= 12)
        {
            return EXT_SCOPE_FIGURE;
        }
    }
    return EXT_SCOPE_NONE;
}

/* The valid file at path into buf; its size, 0 when absent. */
static unsigned tbs_load(const char *path, uint8_t *buf)
{
    FIL f;
    UINT n = 0;
    if (f_open(&f, path, FA_READ) != FR_OK)
    {
        return 0;
    }
    FSIZE_t z = f_size(&f);
    bool ok = z >= 16 && z <= TBS_MAX && f_read(&f, buf, (UINT)z, &n) == FR_OK && n == z;
    f_close(&f);
    return ok && ext_tbs_valid(buf, n) ? n : 0;
}

static bool tbs_store(const char *path, const uint8_t *data, unsigned n)
{
    FIL f;
    UINT w = 0;
    if (!sd_make_dirs(path) || f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return false;
    }
    bool ok = f_write(&f, data, n, &w) == FR_OK && w == n;
    ok = f_close(&f) == FR_OK && ok;
    generation++;
    return ok;
}

static bool load_kind(const uint8_t uid[8], uint8_t kind, uint8_t *buf)
{
    char path[30];
    ext_path(uid, path);
    return tbs_load(path, buf) && buf[4] == kind;
}

bool ext_profile(const uint8_t uid[8], ext_settings_t *out)
{
    uint8_t buf[TBS_MAX];
    if (!load_kind(uid, EXT_PROFILE, buf))
    {
        return false;
    }
    ext_parse_settings(buf, out);
    return true;
}

bool ext_event_tag(const uint8_t uid[8], ext_event_t *out)
{
    uint8_t buf[TBS_MAX];
    if (!load_kind(uid, EXT_EVENT, buf))
    {
        return false;
    }
    ext_parse_event(buf, out);
    return true;
}

bool ext_global(ext_settings_t *out)
{
    uint8_t buf[TBS_MAX];
    if (!load_kind(NULL, EXT_GLOBAL, buf))
    {
        return false;
    }
    ext_parse_settings(buf, out);
    return true;
}

/* ascending keys, 2 bytes for key 0x03, 1 byte otherwise;
 * keys not allowed in the kind are left out. */
unsigned ext_build(uint8_t kind, uint32_t stamp, const ext_settings_t *s, uint8_t *out)
{
    unsigned n = 12;
    for (unsigned k = 1; k <= 10; k++)
    {
        if (!(s->present & (1u << k)) || !(keys[k].kinds & (1u << kind)))
        {
            continue;
        }
        out[n++] = (uint8_t)k;
        out[n++] = keys[k].len;
        out[n++] = (uint8_t)s->value[k];
        if (keys[k].len == 2)
        {
            out[n++] = (uint8_t)(s->value[k] >> 8);
        }
    }
    memcpy(out, "TBS1", 4);
    out[4] = kind;
    out[5] = 0; /* origin: the box */
    out[6] = (uint8_t)(n - 12);
    out[7] = 0;
    ext_put32(out + 8, stamp);
    ext_put32(out + n, crc32_ogg(0, out, n));
    return n + 4;
}

void ext_pack_report(const ext_settings_t *s, uint32_t *packed, uint32_t *values)
{
    uint32_t p = s->kind & 3u, v = 0;
    for (unsigned k = 1; k <= 7; k++)
    {
        if (s->present & (1u << k))
        {
            p |= 1u << (3 + k);
        }
    }
    if (ext_has(s, EXT_RESUME))
    {
        p |= (uint32_t)(s->value[EXT_RESUME] & 1) << 12;
    }
    if (ext_has(s, EXT_SHUFFLE))
    {
        p |= (uint32_t)(s->value[EXT_SHUFFLE] & 1) << 13;
    }
    if (ext_has(s, EXT_LED))
    {
        p |= (uint32_t)(s->value[EXT_LED] & 3) << 14;
    }
    if (ext_has(s, EXT_BOTH_EARS))
    {
        p |= (uint32_t)(s->value[EXT_BOTH_EARS] & 1) << 16;
    }
    if (ext_has(s, EXT_SLEEP))
    {
        v |= s->value[EXT_SLEEP];
    }
    if (ext_has(s, EXT_SPEAKER_LIMIT))
    {
        v |= (uint32_t)(s->value[EXT_SPEAKER_LIMIT] & 15) << 16;
    }
    if (ext_has(s, EXT_HEADPHONE_LIMIT))
    {
        v |= (uint32_t)(s->value[EXT_HEADPHONE_LIMIT] & 15) << 20;
    }
    for (unsigned i = 0; i < 3; i++)
    {
        if (ext_has(s, (ext_key_t)(EXT_SLAP_LOCK + i)))
        {
            v |= 1u << (24 + i) | (uint32_t)(s->value[EXT_SLAP_LOCK + i] & 1) << (27 + i);
        }
    }
    *packed = p;
    *values = v;
}

bool ext_write(const uint8_t uid[8], ext_settings_t *s)
{
    static const uint8_t zero[8];
    uint8_t kind = uid && memcmp(uid, zero, 8) ? EXT_PROFILE : EXT_GLOBAL;
    uint8_t file[64];
    char path[30];
    s->kind = kind;
    s->stamp = clock_now();
    unsigned n = ext_build(kind, s->stamp, s, file);
    ext_path(uid, path);
    bool ok = tbs_store(path, file, n);
    debug_printf("ext: %s written, %u B%s\n", path, n, ok ? "" : " FAILED");
    if (ok && hello)
    {
        /* the settings report, only after the hello of this start */
        uint32_t packed, values;
        ext_pack_report(s, &packed, &values);
        log_args_t a = {0};
        log_arg_num(&a, kind == EXT_PROFILE ? (int32_t)ext_le32(uid) : 0);
        log_arg_num(&a, kind == EXT_PROFILE ? (int32_t)ext_le32(uid + 4) : 0);
        log_arg_num(&a, (int32_t)packed);
        log_arg_num(&a, (int32_t)values);
        log_arg_num(&a, (int32_t)s->stamp);
        log_fn(LOG_INFO, 22, 32258, 0, &a);
    }
    return ok;
}

/* --- the freshness-check answer --- */

static bool ext_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    for (unsigned sh = 0; sh < 70; sh += 7)
    {
        if (*p >= end)
        {
            return false;
        }
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80))
        {
            *v = r;
            return true;
        }
    }
    return false;
}

/* one field 100; true when the file was written. */
static bool store_field(const uint8_t *d, unsigned len)
{
    static const uint8_t zero[8];
    if (len <= 8)
    {
        return false;
    }
    const uint8_t *f = d + 8;
    unsigned n = len - 8;
    bool global = !memcmp(d, zero, 8);
    if (!ext_tbs_valid(f, n) || (f[4] == EXT_GLOBAL) != global)
    {
        debug_printf("ext: field 100 refused\n");
        return false;
    }
    char path[30];
    uint8_t held[TBS_MAX];
    ext_path(d, path);
    /* the newer one wins; an equal time stamp is the same file */
    if (tbs_load(path, held) && ext_le32(f + 8) <= ext_le32(held + 8))
    {
        return false;
    }
    bool ok = tbs_store(path, f, n);
    debug_printf("ext: %s stored, %u B%s\n", path, n, ok ? "" : " FAILED");
    return ok;
}

/* F12 command 64: the arguments are a field 100. */
bool ext_store(const uint8_t *field, unsigned len)
{
    return store_field(field, len);
}

static void hello_answer(void)
{
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)CFW_BUILD_TIME);
    log_arg_num(&a, (int32_t)EXT_CAPS);
    log_arg_num(&a, (int32_t)boot_result);
    log_arg_num(&a, (int32_t)boot_rolled_back);
    log_fn(LOG_INFO, 22, 32257, 0, &a);
    answered = true;
    debug_printf("ext: hello, capabilities 0x%04lx\n", (unsigned long)EXT_CAPS);
}

void ext_answer(const uint8_t *p, unsigned len)
{
    const uint8_t *end = p + len;
    while (p < end)
    {
        uint64_t key, v;
        if (!ext_varint(&p, end, &key))
        {
            return;
        }
        unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
        /* Fields 1 to 8 are walked as the cloud reads them, whatever wire
         * type they carry, so that the walk stays in step with it. */
        if (field == 1 && wire != 2)
        {
            if (end - p < 8)
            {
                return;
            }
            p += 8;
            continue;
        }
        if (field >= 2 && field <= 8)
        {
            if (!ext_varint(&p, end, &v))
            {
                return;
            }
            continue;
        }
        switch (wire)
        {
        case 0:
            if (!ext_varint(&p, end, &v))
            {
                return;
            }
            if (field == 101 && !hello)
            {
                hello = true;
                if (trial_waiting)
                {
                    answer_due = true;
                    debug_printf("ext: hello after trial confirmed\n");
                }
                else
                {
                    hello_answer();
                }
            }
            break;
        case 1:
            if (end - p < 8)
            {
                return;
            }
            p += 8;
            break;
        case 2:
            if (!ext_varint(&p, end, &v) || v > (uint64_t)(end - p))
            {
                return;
            }
            if (field == 100)
            {
                store_field(p, (unsigned)v);
            }
            p += v;
            break;
        case 5:
            if (end - p < 4)
            {
                return;
            }
            p += 4;
            break;
        default:
            return;
        }
    }
}

/* --- state file --- */

void ext_state_pack(const ext_state_t *s, uint8_t r[32])
{
    memcpy(r, s->uid, 8);
    ext_put32(r + 8, s->sequence);
    ext_put32(r + 12, s->audio_id);
    ext_put32(r + 16, s->position);
    ext_put32(r + 20, s->seed);
    r[24] = s->index;
    r[25] = s->chapters;
    r[26] = s->flags & EXT_STATE_SHUFFLING;
    memset(r + 27, 0, 4);
    uint8_t c = 0xA5;
    for (int i = 0; i < 31; i++)
    {
        c ^= r[i];
    }
    r[31] = c;
}

bool ext_state_unpack(const uint8_t r[32], ext_state_t *s)
{
    uint8_t c = 0xA5;
    for (int i = 0; i < 31; i++)
    {
        c ^= r[i];
    }
    memcpy(s->uid, r, 8);
    s->sequence = ext_le32(r + 8);
    s->audio_id = ext_le32(r + 12);
    s->position = ext_le32(r + 16);
    s->seed = ext_le32(r + 20);
    s->index = r[24];
    s->chapters = r[25];
    s->flags = r[26];
    return s->sequence && c == r[31];
}

static bool state_open(FIL *f, bool write)
{
    if (f_open(f, STATE_PATH, write ? FA_READ | FA_WRITE : FA_READ) != FR_OK)
    {
        return false;
    }
    if (f_size(f) == STATE_RECORDS * 32)
    {
        return true;
    }
    f_close(f);
    return false;
}

bool ext_state_get(const uint8_t uid[8], ext_state_t *out)
{
    FIL f;
    if (!state_open(&f, false))
    {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < STATE_RECORDS; i++)
    {
        uint8_t r[32];
        UINT n;
        ext_state_t s;
        if (f_read(&f, r, 32, &n) != FR_OK || n != 32)
        {
            break;
        }
        if (ext_state_unpack(r, &s) && !memcmp(s.uid, uid, 8) && (!found || s.sequence > out->sequence))
        {
            *out = s;
            found = true;
        }
    }
    f_close(&f);
    return found;
}

bool ext_state_put(ext_state_t *s)
{
    FIL f;
    UINT n;
    uint8_t r[32];
    if (!state_open(&f, true))
    {
        /* the first write creates the file at its full size, all zero */
        memset(r, 0, sizeof r);
        if (!sd_make_dirs(STATE_PATH) || f_open(&f, STATE_PATH, FA_READ | FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        {
            return false;
        }
        bool ok = true;
        for (unsigned i = 0; i < STATE_RECORDS && ok; i++)
        {
            ok = f_write(&f, r, 32, &n) == FR_OK && n == 32;
        }
        if (!ok)
        {
            f_close(&f);
            return false;
        }
    }
    int same = -1, free_rec = -1, oldest = 0;
    uint32_t high = 0, low = 0xFFFFFFFFu, same_seq = 0;
    bool ok = f_lseek(&f, 0) == FR_OK;
    for (unsigned i = 0; i < STATE_RECORDS && ok; i++)
    {
        ext_state_t o;
        ok = f_read(&f, r, 32, &n) == FR_OK && n == 32;
        if (!ok)
        {
            break;
        }
        if (!ext_state_unpack(r, &o))
        {
            if (free_rec < 0)
            {
                free_rec = (int)i;
            }
            continue;
        }
        if (o.sequence > high)
        {
            high = o.sequence;
        }
        if (o.sequence < low)
        {
            low = o.sequence;
            oldest = (int)i;
        }
        if (!memcmp(o.uid, s->uid, 8) && o.sequence >= same_seq)
        {
            same = (int)i;
            same_seq = o.sequence;
        }
    }
    int at = same >= 0 ? same : free_rec >= 0 ? free_rec
                                              : oldest;
    s->sequence = high + 1;
    ext_state_pack(s, r);
    ok = ok && f_lseek(&f, (FSIZE_t)at * 32) == FR_OK && f_write(&f, r, 32, &n) == FR_OK && n == 32;
    ok = f_close(&f) == FR_OK && ok;
    return ok;
}

/* --- shuffle order --- */

static uint32_t mix(uint32_t x, uint32_t k, uint32_t r)
{
    x = x * 0x9E3779B1u + k + r * 0x7F4A7C15u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    return x ^ (x >> 12);
}

void ext_shuffle(uint32_t seed, unsigned n, uint8_t *order)
{
    uint32_t x = mix(seed, seed >> 16, 5) | 1;
    for (unsigned i = 0; i < n; i++)
    {
        order[i] = (uint8_t)i;
    }
    for (unsigned i = n - 1; i >= 1 && n; i--)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        unsigned j = (unsigned)(((uint64_t)x * (i + 1)) >> 32);
        uint8_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}
