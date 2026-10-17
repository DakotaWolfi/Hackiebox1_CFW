/*
 * Content files: header block, block check, completeness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ff.h"
#include "taf.h"

/* --- header --- */

/* Reads a varint of up to 10 bytes below end; false when it runs past end
 * or is longer. */
static bool varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    for (unsigned shift = 0; shift < 70; shift += 7)
    {
        if (*p >= end)
        {
            return false;
        }
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
        {
            *v = r;
            return true;
        }
    }
    return false;
}

static bool varint32(const uint8_t **p, const uint8_t *end, uint32_t *v)
{
    uint64_t w;
    if (!varint(p, end, &w) || w > 0xFFFFFFFFu)
    {
        return false;
    }
    *v = (uint32_t)w;
    return true;
}

/* Field 4 (and 7, only counted): a packed run when the field has wire type
 * 2, else one varint. */
static bool repeated(const uint8_t **p, const uint8_t *end, unsigned wire, uint32_t *values, unsigned *count,
                     unsigned max)
{
    const uint8_t *stop = end;
    if (wire == 2)
    {
        uint64_t len;
        if (!varint(p, end, &len) || len > (uint64_t)(end - *p))
        {
            return false;
        }
        stop = *p + len;
    }
    do
    {
        /* Field 7 entries are signed 32-bit values: a negative one is a
         * ten-byte varint, so only field 4 is limited to 32 bits. */
        uint64_t v;
        if (!varint(p, stop, &v) || *count >= max || (values && v > 0xFFFFFFFFu))
        {
            return false;
        }
        if (values)
        {
            values[*count] = (uint32_t)v;
        }
        ++*count;
    } while (wire == 2 && *p < stop);
    return true;
}

bool taf_header_decode(const uint8_t *block, taf_header_t *h)
{
    if (block[0] || block[1] || block[2] != 0x0F || block[3] != 0xFC)
    {
        return false; /* length 4092, big-endian */
    }
    return taf_header_decode_n(block + 4, TAF_BLOCK - 4, h);
}

bool taf_header_decode_n(const uint8_t *msg, unsigned len, taf_header_t *h)
{
    const uint8_t *p = msg, *end = msg + len;
    h->set = 1; /* field 6 absent */
    bool have1 = false, have2 = false, have3 = false;
    unsigned chapters = 0, seven = 0;
    uint32_t chapter[TAF_MAX_CHAPTERS];

    while (p < end && *p)
    {
        uint64_t key;
        if (!varint(&p, end, &key))
        {
            return false;
        }
        unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
        uint32_t v;
        uint64_t len;
        switch (field)
        {
        case 1:
            if (!varint(&p, end, &len) || len > 20 || len > (uint64_t)(end - p))
            {
                return false;
            }
            p += len;
            have1 = true;
            break;
        case 2:
            if (!varint32(&p, end, &h->num_bytes))
            {
                return false;
            }
            have2 = true;
            break;
        case 3:
            if (!varint32(&p, end, &h->audio_id))
            {
                return false;
            }
            have3 = true;
            break;
        case 4:
            if (!repeated(&p, end, wire, chapter, &chapters, TAF_MAX_CHAPTERS))
            {
                return false;
            }
            break;
        case 6:
            if (!varint32(&p, end, &v))
            {
                return false;
            }
            h->set = v;
            break;
        case 7:
            if (!repeated(&p, end, wire, NULL, &seven, 8))
            {
                return false;
            }
            break;
        default:
            switch (wire)
            {
            case 0:
                if (!varint(&p, end, &len))
                {
                    return false;
                }
                break;
            case 1:
                if (end - p < 8)
                {
                    return false;
                }
                p += 8;
                break;
            case 2:
                if (!varint(&p, end, &len) || len > (uint64_t)(end - p))
                {
                    return false;
                }
                p += len;
                break;
            case 5:
                if (end - p < 4)
                {
                    return false;
                }
                p += 4;
                break;
            default:
                return false;
            }
        }
    }
    if (!have1 || !have2 || !have3)
    {
        return false;
    }
    h->chapters = chapters;
    memcpy(h->chapter, chapter, chapters * sizeof chapter[0]);
    return true;
}

/* --- Ogg pages --- */

uint32_t taf_page_crc(const uint8_t *page, unsigned n)
{
    uint32_t crc = 0;
    for (unsigned i = 0; i < n; i++)
    {
        uint8_t b = (i >= 22 && i < 26) ? 0 : page[i];
        crc ^= (uint32_t)b << 24;
        for (int k = 0; k < 8; k++)
        {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool taf_block_ok(const uint8_t *data, unsigned n, uint32_t x, uint32_t audio_id)
{
    unsigned pos = 0;
    while (pos < n)
    {
        const uint8_t *pg = data + pos;
        if (n - pos < 27 || memcmp(pg, "OggS", 4))
        {
            return false;
        }
        if (audio_id && le32(pg + 14) != audio_id)
        {
            return false;
        }
        uint32_t seq = le32(pg + 18);
        if (!((x == TAF_BLOCK && seq <= 1) || seq == x / TAF_BLOCK + 1))
        {
            return false;
        }
        unsigned segs = pg[26];
        if (n - pos < 27 + segs)
        {
            return false;
        }
        unsigned size = 27 + segs;
        for (unsigned i = 0; i < segs; i++)
        {
            size += pg[27 + i];
        }
        if (size > n - pos || taf_page_crc(pg, size) != le32(pg + 22))
        {
            return false;
        }
        pos += size;
    }
    return true;
}

/* --- opening --- */

taf_state_t taf_open(FIL *f, const char *path, taf_header_t *h, uint8_t *buf, bool ignore_mark)
{
    FILINFO fi;
    FRESULT r = f_stat(path, &fi);
    if (r == FR_NO_FILE || r == FR_NO_PATH || (r == FR_OK && (fi.fattrib & AM_DIR)))
    {
        return TAF_MISSING;
    }
    if (r != FR_OK)
    {
        return TAF_READ_ERROR;
    }
    /* The outdated mark: also set during a download (cloud.c, prepare), so
     * a marked file whose end fails the check is incomplete and resumed
     * (the request's If-Range brings the whole new content when it has
     * changed); a marked file that looks complete is outdated. */
    bool marked = (fi.fattrib & AM_HID) && !ignore_mark;
    if (f_open(f, path, FA_READ) != FR_OK)
    {
        return TAF_READ_ERROR;
    }
    UINT got;
    if (f_read(f, buf, TAF_BLOCK, &got) != FR_OK)
    {
        f_close(f);
        return TAF_READ_ERROR;
    }
    uint32_t z = (uint32_t)f_size(f);
    if (got < TAF_BLOCK || !taf_header_decode(buf, h) || z != TAF_BLOCK + h->num_bytes)
    {
        f_close(f);
        return TAF_INVALID;
    }
    uint32_t l = z & ~(TAF_BLOCK - 1);
    if (l < 2 * TAF_BLOCK)
    {
        f_close(f);
        return TAF_INVALID;
    }
    if (l != z &&
        (f_lseek(f, l) != FR_OK || f_read(f, buf, z - l, &got) != FR_OK || got != z - l))
    {
        f_close(f);
        return TAF_READ_ERROR;
    }
    if (l != z && !taf_block_ok(buf, got, l, h->audio_id))
    {
        return TAF_INCOMPLETE;
    }
    if (marked)
    {
        f_close(f);
        return TAF_OUTDATED;
    }
    return TAF_COMPLETE;
}

static bool block_valid(FIL *f, uint32_t x, uint32_t z, uint32_t audio_id, uint8_t *buf)
{
    UINT got;
    uint32_t n = z - x < TAF_BLOCK ? z - x : TAF_BLOCK;
    return f_lseek(f, x) == FR_OK && f_read(f, buf, n, &got) == FR_OK && got == n &&
           taf_block_ok(buf, n, x, audio_id);
}

uint32_t taf_resume_offset(FIL *f, uint32_t z, uint32_t audio_id, uint8_t *buf)
{
    uint32_t lo = TAF_BLOCK, hi = z & ~(TAF_BLOCK - 1);
    while (hi - lo > TAF_BLOCK)
    {
        uint32_t mid = (lo + (hi - lo) / 2) & ~(TAF_BLOCK - 1);
        if (block_valid(f, mid, z, audio_id, buf))
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return lo > 2 * TAF_BLOCK ? lo : 0;
}
