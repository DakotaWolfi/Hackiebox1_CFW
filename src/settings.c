/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "debug.h"
#include "flashfs.h"
#include "port.h"
#include "settings.h"

settings_t settings = {
    .limit_speaker = 3,
    .limit_headphones = 3,
    .slap_enable = 1,
    .slap_direction = 1,
    .reserved6 = 0,
    .led = 0,
};

static uint8_t language;
static uint32_t archive_audio_id = 1;
/* fields 5 and 6 of /settings.cfg as last read */
static char country[2];
static uint8_t country_len = 0xFF; /* 0xFF: field 5 absent */
static uint32_t factory;
static bool have_factory;

uint8_t settings_language(void)
{
    return language;
}

uint32_t settings_archive_audio_id(void)
{
    return archive_audio_id;
}

#if !PORT_LANGUAGE_KEY
static long flash_read(const char *name, uint8_t *buf, unsigned max)
{
    return flashfs_read(name, 0, buf, max);
}

static bool varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
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

/* field 1 of /settings.cfg; false when it does not decode. */
static bool settings_file(uint32_t *index)
{
    uint8_t buf[40];
    country_len = 0xFF;
    have_factory = false;
    long n = flash_read("/settings.cfg", buf, sizeof buf);
    if (n <= 0)
    {
        return false;
    }
    const uint8_t *p = buf, *end = buf + n;
    bool have1 = false;
    while (p < end && *p)
    {
        uint64_t key, v;
        if (!varint(&p, end, &key))
        {
            return false;
        }
        unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
        if (wire == 0)
        {
            if (!varint(&p, end, &v))
            {
                return false;
            }
            if ((field == 1 || field == 6) && v > 0xFFFFFFFFu)
            {
                return false;
            }
            if (field >= 2 && field <= 4 && ((int32_t)(uint32_t)v < -128 || (int32_t)(uint32_t)v > 127))
            {
                return false;
            }
            if (field == 1)
            {
                *index = (uint32_t)v;
                have1 = true;
            }
            else if (field == 6)
            {
                factory = (uint32_t)v;
                have_factory = true;
            }
        }
        else if (wire == 2)
        {
            if (!varint(&p, end, &v) || v > (uint64_t)(end - p) || (field == 5 && v > 2))
            {
                return false;
            }
            if (field == 5)
            {
                memcpy(country, p, (size_t)v);
                country_len = (uint8_t)v;
            }
            p += v;
        }
        else
        {
            return false;
        }
    }
    return have1;
}
#endif

/* A 128-byte window over a flash file, for parsing headers in place. */
static struct
{
    uint32_t base, len;
    uint8_t buf[128];
} win;

static int at(uint32_t off)
{
    if (off < win.base || off >= win.base + win.len)
    {
        long n = flashfs_read("/fw/sfx.bin", off, win.buf, sizeof win.buf);
        if (n <= 0)
        {
            return -1;
        }
        win.base = off;
        win.len = (uint32_t)n;
    }
    return win.buf[off - win.base];
}

static bool wvarint(uint32_t *off, uint32_t end, uint64_t *v)
{
    uint64_t r = 0;
    for (unsigned sh = 0; sh < 70; sh += 7)
    {
        if (*off >= end)
        {
            return false;
        }
        int b = at((*off)++);
        if (b < 0)
        {
            return false;
        }
        r |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80))
        {
            *v = r;
            return true;
        }
    }
    return false;
}

/* The first entry header of
 * the archive, decoded where it lies (H bytes after the length). */
static bool archive_header(uint32_t *audio_id, uint32_t *set)
{
    long size = flashfs_size("/fw/sfx.bin");
    if (size < 0)
    {
        return false;
    }
    win.len = 0;
    bool ok = false;
    uint32_t h = 0;
    int b0 = at(0), b1 = at(1), b2 = at(2), b3 = at(3);
    if (b0 >= 0 && b1 >= 0 && b2 >= 0 && b3 >= 0)
    {
        h = (uint32_t)b0 << 24 | (uint32_t)b1 << 16 | (uint32_t)b2 << 8 | (uint32_t)b3;
        uint32_t read = size < 4096 ? (uint32_t)size : 4096;
        ok = 4ull + h <= read;
    }
    uint32_t off = 4, end = 4 + h;
    bool have1 = false, have2 = false, have3 = false;
    *set = 1;
    while (ok && off < end)
    {
        int c = at(off);
        if (c == 0)
        {
            break;
        }
        uint64_t key, v;
        if (!wvarint(&off, end, &key))
        {
            ok = false;
            break;
        }
        unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
        if (field == 1 || wire == 2)
        {
            if (!wvarint(&off, end, &v) || v > end - off || (field == 1 && v > 20))
            {
                ok = false;
            }
            off += (uint32_t)v;
            have1 |= field == 1;
        }
        else if (wire == 0)
        {
            if (!wvarint(&off, end, &v) || ((field == 2 || field == 3 || field == 6) && v > 0xFFFFFFFFu))
            {
                ok = false;
            }
            if (field == 2)
            {
                have2 = true;
            }
            if (field == 3)
            {
                *audio_id = (uint32_t)v;
                have3 = true;
            }
            if (field == 6)
            {
                *set = (uint32_t)v;
            }
        }
        else if (wire == 1 || wire == 5)
        {
            off += wire == 1 ? 8 : 4;
            ok = off <= end;
        }
        else
        {
            ok = false;
        }
    }
    return ok && have1 && have2 && have3 && *set <= 3;
}

static volatile bool ready;

bool settings_ready(void)
{
    return ready;
}

void settings_load(void)
{
    uint8_t default_language = 0;
    uint32_t id = 1, set = 1;
    if (archive_header(&id, &set))
    {
        archive_audio_id = id;
        default_language = (uint8_t)set;
    }
    else
    {
        debug_printf("settings: sfx.bin bad\n");
    }
#if PORT_LANGUAGE_KEY
    /* a key of its own, default the archive's first set. */
    uint8_t stored = 0xFF;
    uint32_t index = port_language_read(&stored) ? stored : 0xFF;
    bool decoded = index <= 3;
#else
    /* The language from /settings.cfg (the rewrite of the file follows). */
    uint32_t index;
    bool decoded = settings_file(&index);
    if (!decoded)
    {
        country_len = 0xFF; /* EU, factory index := index */
        have_factory = false;
    }
#endif
    if (decoded && index <= 3)
    {
        language = (uint8_t)index;
    }
    else
    {
        language = default_language;
#if PORT_LANGUAGE_KEY
        debug_printf("settings: no language stored\n");
#else
        debug_printf("settings: settings.cfg bad\n");
#endif
    }
    debug_printf("settings: language %u, sfx %lu\n", language, (unsigned long)archive_audio_id);
    ready = true;
}

/* the country code of the settings file; false when the file
 * holds none (then EU applies). */
bool settings_country(char cc[2])
{
    if (country_len == 0xFF)
    {
        return false;
    }
    cc[0] = country_len > 0 ? country[0] : 0;
    cc[1] = country_len > 1 ? country[1] : 0;
    return true;
}

/* fields 1, 5 and 6 in ascending order, fields
 * 2 to 4 left out; at most 40 bytes, written in one write. */
unsigned settings_encode(uint8_t l, uint8_t *out)
{
    unsigned n = 0;
    out[n++] = 0x08;
    out[n++] = l;
    unsigned cl = country_len == 0xFF ? 2 : country_len;
    out[n++] = 0x2A;
    out[n++] = (uint8_t)cl;
    memcpy(out + n, country_len == 0xFF ? "EU" : country, cl);
    n += cl;
    uint32_t f = have_factory ? factory : l;
    out[n++] = 0x30;
    do
    {
        out[n++] = (uint8_t)((f & 0x7F) | (f > 0x7F ? 0x80 : 0));
        f >>= 7;
    } while (f);
    return n;
}

/* The new language index, keeping the
 * country code and the factory language index. */
void settings_set_language(uint8_t l)
{
    if (l > 3)
    {
        return; /* never stored */
    }
#if PORT_LANGUAGE_KEY
    if (!port_language_write(l))
    {
        debug_printf("settings: language not stored\n");
        return;
    }
#else
    uint8_t msg[16];
    unsigned n = settings_encode(l, msg);
    if (!have_factory)
    {
        factory = l;
        have_factory = true;
    }
    if (country_len == 0xFF)
    {
        memcpy(country, "EU", 2);
        country_len = 2;
    }
    long r = flashfs_write("/settings.cfg", msg, n);
    if (r != (long)n)
    {
        debug_printf("settings: write %ld\n", r);
        return;
    }
#endif
    language = l;
    debug_verbose("settings: language %u stored\n", l);
}
