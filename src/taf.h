/*
 * Content files: header block, block check, completeness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TAF_H
#define TAF_H

#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

#define TAF_BLOCK 4096u
#define TAF_MAX_CHAPTERS 99

typedef struct
{
    uint32_t num_bytes;
    uint32_t audio_id;
    unsigned chapters;
    uint32_t chapter[TAF_MAX_CHAPTERS]; /* page numbers */
    uint32_t set;                       /* field 6, 1 when absent */
} taf_header_t;

/* decodes the header block (4096 bytes). */
bool taf_header_decode(const uint8_t *block, taf_header_t *h);

/* The same rules for a message of len bytes (archive entries). */
bool taf_header_decode_n(const uint8_t *msg, unsigned len, taf_header_t *h);

/* the data read for the block at file offset x (n bytes,
 * 4096 or fewer at the end of the file). */
bool taf_block_ok(const uint8_t *data, unsigned n, uint32_t x, uint32_t audio_id);

uint32_t taf_page_crc(const uint8_t *page, unsigned n);

typedef enum
{
    TAF_COMPLETE,
    TAF_INCOMPLETE,
    TAF_MISSING,
    TAF_OUTDATED, /* outdated mark (check step 2) */
    TAF_INVALID,  /* check steps 3 to 6, or L below 8192 */
    TAF_READ_ERROR,
} taf_state_t;

/* The content checks for a figure being placed: opens the file,
 * decodes the header and checks the completeness. On TAF_COMPLETE and
 * TAF_INCOMPLETE the file stays open. buf is 4096 bytes of work space. */
taf_state_t taf_open(FIL *f, const char *path, taf_header_t *h, uint8_t *buf, bool ignore_mark);

/* resume offset of an incomplete file (open, size z). */
uint32_t taf_resume_offset(FIL *f, uint32_t z, uint32_t audio_id, uint8_t *buf);

#endif
