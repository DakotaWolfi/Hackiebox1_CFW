/*
 * Host tool: encodes a test melody (48 kHz stereo, CELT-only, 20 ms frames)
 * and writes the Ogg stream of a content file with the page layout of
 * pages 0 and 1 and the first audio page in the first 4096-byte
 * block, then one page per block, each block filled exactly by padding the
 * last packet of the page. mktaf.py adds the header block.
 *
 * usage: mkogg <out.ogg> <serial> [seconds [chapter seconds]]
 *
 * With chapter seconds, the melody is transposed up by a fourth in each
 * chapter and the sequence number of the page that holds the first packet
 * of each chapter after the first is printed ("chapter page N").
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"

#define RATE 48000
#define FRAME 960
#define BLOCK 4096
#define MAXP 1275

static uint32_t serial;
static uint32_t seq;
static FILE *out;
static long chapter_frames;
static long written;

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        crc_table[i] = r;
    }
}

static void write_page(int flags, uint64_t granule, uint8_t **pk, int *len, int n)
{
    uint8_t page[BLOCK + 300];
    int segs = 0, body = 0;
    uint8_t lacing[255];
    for (int i = 0; i < n; i++) {
        int l = len[i];
        while (l >= 255) {
            lacing[segs++] = 255;
            l -= 255;
        }
        lacing[segs++] = (uint8_t)l;
        body += len[i];
    }
    memcpy(page, "OggS", 4);
    page[4] = 0;
    page[5] = (uint8_t)flags;
    for (int i = 0; i < 8; i++)
        page[6 + i] = (uint8_t)(granule >> (8 * i));
    for (int i = 0; i < 4; i++) {
        page[14 + i] = (uint8_t)(serial >> (8 * i));
        page[18 + i] = (uint8_t)(seq >> (8 * i));
        page[22 + i] = 0;
    }
    page[26] = (uint8_t)segs;
    memcpy(page + 27, lacing, segs);
    int pos = 27 + segs;
    for (int i = 0; i < n; i++) {
        memcpy(page + pos, pk[i], len[i]);
        pos += len[i];
    }
    uint32_t crc = 0;
    for (int i = 0; i < pos; i++)
        crc = (crc << 8) ^ crc_table[((crc >> 24) ^ page[i]) & 0xFF];
    for (int i = 0; i < 4; i++)
        page[22 + i] = (uint8_t)(crc >> (8 * i));
    fwrite(page, 1, pos, out);
    written += pos;
    seq++;
}

static int segs_of(int l)
{
    return l / 255 + 1;
}

static int page_size(int *len, int n)
{
    int s = 27;
    for (int i = 0; i < n; i++)
        s += segs_of(len[i]) + len[i];
    return s;
}

/* Melody: a scale up and down, 250 ms per note, left/right panning. */
static void synth(int16_t *pcm, long frame_index)
{
    static const double notes[] = { 261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25,
                                    493.88, 440.00, 392.00, 349.23, 329.63, 293.66, 261.63, 0 };
    for (int i = 0; i < FRAME; i++) {
        long t = frame_index * FRAME + i;
        int note = (int)((t / (RATE / 4)) % 16);
        double f = notes[note];
        if (chapter_frames)
            f *= pow(2.0, (double)(frame_index / chapter_frames) * 5.0 / 12.0);
        double phase = (double)(t % (RATE / 4)) / (RATE / 4);
        double env = phase < 0.05 ? phase / 0.05 : (1.0 - phase) * 1.05;
        double v = f ? sin(2 * M_PI * f * t / RATE) * env * 9000 : 0;
        double pan = 0.5 + 0.5 * sin(2 * M_PI * t / (RATE * 4.0));
        pcm[2 * i] = (int16_t)(v * (1 - pan * 0.8));
        pcm[2 * i + 1] = (int16_t)(v * (0.2 + pan * 0.8));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    out = fopen(argv[1], "wb");
    serial = (uint32_t)strtoul(argv[2], NULL, 0);
    int seconds = argc > 3 ? atoi(argv[3]) : 12;
    chapter_frames = argc > 4 ? (long)atoi(argv[4]) * RATE / FRAME : 0;
    long next_chapter = chapter_frames;
    crc_init();

    int err;
    OpusEncoder *enc = opus_encoder_create(RATE, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK)
        return 1;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    int lookahead;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    uint8_t head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2 };
    head[10] = (uint8_t)lookahead;
    head[11] = (uint8_t)(lookahead >> 8);
    uint32_t rate = RATE;
    memcpy(head + 12, &rate, 4);
    uint8_t *p0[1] = { head };
    int l0[1] = { 19 };
    write_page(2, 0, p0, l0, 1);
    static uint8_t tags[] = "OpusTags\x08\x00\x00\x00" "cfw test\x00\x00\x00\x00";
    uint8_t *p1[1] = { tags };
    int l1[1] = { 24 };
    write_page(0, 0, p1, l1, 1);

    long frames_total = (long)seconds * RATE / FRAME;
    static uint8_t buf[256][MAXP + 300];
    uint8_t *pk[256];
    int len[256], n = 0;
    int16_t pcm[2 * FRAME];
    uint64_t granule = 0;
    long f = 0;
    int have_next = 0, next_len = 0;
    static uint8_t next[MAXP];

    while (f < frames_total || have_next || n) {
        if (!have_next && f < frames_total) {
            synth(pcm, f++);
            next_len = opus_encode(enc, pcm, FRAME, next, MAXP);
            have_next = 1;
        }
        long room = BLOCK - written % BLOCK;
        if (have_next) {
            len[n] = next_len;
            int ps = page_size(len, n + 1);
            if ((ps == room || ps <= room - 3) && n < 200) {
                if (chapter_frames && f - 1 == next_chapter) {
                    printf("chapter page %u\n", seq);
                    next_chapter += chapter_frames;
                }
                memcpy(buf[n], next, next_len);
                pk[n] = buf[n];
                n++;
                granule += FRAME;
                have_next = 0;
                continue;
            }
        }
        /* Close the page: pad the last packet to fill the block exactly. */
        int last = n - 1;
        int orig = len[last], ok = 0;
        for (int target = orig; target <= orig + room; target++) {
            len[last] = target;
            if (page_size(len, n) != room)
                continue;
            if (target == orig || opus_packet_pad(buf[last], orig, target) == OPUS_OK) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            fprintf(stderr, "cannot fill block at %ld\n", written);
            return 1;
        }
        int eos = (f >= frames_total && !have_next) ? 4 : 0;
        write_page(eos, granule, pk, len, n);
        n = 0;
    }
    fclose(out);
    fprintf(stderr, "%ld bytes, %u pages, pre-skip %d\n", written, seq, lookahead);
    return 0;
}
