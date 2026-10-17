/*
 * Host tool: decodes a content file like the box (16 kHz stereo, with the
 * Opus pre-skip) into a WAV file for listening and comparison.
 * usage: dectaf <in.taf> <out.wav>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "opus.h"

int main(int argc, char **argv)
{
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    static uint8_t blk[4096], pkt[8192];
    static int16_t pcm[2 * 1920];
    int err, index = 0, plen = 0;
    long total = 0, toc_bad = 0;
    unsigned preskip = 0;
    OpusDecoder *dec = opus_decoder_create(16000, 2, &err);
    fseek(out, 44, SEEK_SET);
    fseek(in, 4096, SEEK_SET);
    size_t n;
    while ((n = fread(blk, 1, 4096, in)) > 0) {
        size_t pos = 0;
        while (pos + 27 <= n && !memcmp(blk + pos, "OggS", 4)) {
            uint8_t *pg = blk + pos;
            int segs = pg[26], body = 0;
            for (int i = 0; i < segs; i++)
                body += pg[27 + i];
            uint8_t *d = pg + 27 + segs;
            for (int i = 0; i < segs; i++) {
                int l = pg[27 + i];
                memcpy(pkt + plen, d, l);
                plen += l;
                d += l;
                if (l < 255) {
                    if (index == 0) {
                        preskip = pkt[10] | pkt[11] << 8;
                    } else if (index > 1) {
                        if ((pkt[0] >> 3) < 16)
                            toc_bad++;
                        int f = opus_decode(dec, pkt, plen, pcm, 1920, 0);
                        if (f > 0) {
                            if (preskip >= (unsigned)f) {
                                preskip -= f;
                            } else {
                                if (preskip) {
                                    memmove(pcm, pcm + 2 * preskip, 4 * (f - preskip));
                                    preskip = 0;
                                }
                                fwrite(pcm, 4, f, out);
                                total += f;
                            }
                        }
                    }
                    index++;
                    plen = 0;
                }
            }
            pos += 27 + segs + body;
        }
    }
    uint32_t bytes = total * 4, rate = 16000, br = 64000;
    uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
    uint32_t riff = bytes + 36;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 40, &bytes, 4);
    fseek(out, 0, SEEK_SET);
    fwrite(h, 1, 44, out);
    fclose(out);
    printf("packets %d, frames %ld (%ld ms), non-CELT packets %ld\n", index - 2, total, total / 16, toc_bad);
    return 0;
}
