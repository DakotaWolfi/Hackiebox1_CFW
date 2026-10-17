/*
 * Unpacking of the sound-prompt archive /fw/sfx.bin into the files of
 * language 0. Apart from player.c, as the only
 * user of the SimpleLink file system there: simplelink.h and <stdio.h>
 * cannot be included together without redefinitions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "ff.h"
#include "debug.h"
#include "flashfs.h"
#include "monitor.h"
#include "sdfiles.h"
#include "sfx.h"
#include "taf.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Prompt set 0. */
void sfx_unpack(uint8_t *block)
{
    /* Once per run: a prompt the archive lacks must not rewrite all
     * files at every request. */
    static bool tried;
    if (tried)
    {
        return;
    }
    tried = true;
    if (flashfs_size("/fw/sfx.bin") < 0)
    {
        debug_printf("pl: no /fw/sfx.bin (7067)\n");
        return;
    }
    uint32_t off = 0;
    for (unsigned i = 0; i < 25; i++)
    {
        long n = flashfs_read("/fw/sfx.bin", off, block, TAF_BLOCK);
        taf_header_t h;
        if (n < 4 || 4ull + be32(block) > (unsigned long long)n ||
            !taf_header_decode_n(block + 4, be32(block), &h) || h.set != 0)
        {
            debug_printf("pl: sfx stops at %u (7067)\n", i);
            break;
        }
        uint64_t total = 4ull + be32(block) + h.num_bytes;
        if (h.num_bytes == 0)
        {
            off += (uint32_t)total;
            continue;
        }
        if (total >= 0x100000000ull)
        {
            break;
        }
        char path[32];
        uint8_t uid[8] = {0, 0, 0, 0, 0, 0, 0, (uint8_t)i};
        sd_content_path(uid, path);
        static FIL f;
        if (!sd_make_dirs(path) || (f_unlink(path), f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK)
        {
            debug_printf("pl: %s not created\n", path);
            break;
        }
        bool ok = true;
        for (uint32_t done = 0; done < total && ok;)
        {
            monitor_mark(ACT_SD);
            uint32_t piece = total - done < TAF_BLOCK ? (uint32_t)(total - done) : TAF_BLOCK;
            UINT w;
            ok = flashfs_read("/fw/sfx.bin", off + done, block, piece) == (long)piece && f_write(&f, block, piece, &w) == FR_OK &&
                 w == piece;
            done += piece;
        }
        f_close(&f);
        if (!ok)
        {
            f_unlink(path);
            debug_printf("pl: sfx entry %u not copied\n", i);
            break;
        }
        off += (uint32_t)total;
    }
    debug_printf("pl: prompt archive unpacked\n");
}
