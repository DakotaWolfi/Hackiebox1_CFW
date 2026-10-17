/*
 * The setup page built into the image (card/cfw/setup.htm of this
 * repository): written to /cfw/setup.htm when the card has none, so that
 * setup mode serves it also on a card that has never seen the firmware. A
 * newer page comes from the server as item 7
 * and is then left alone.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include "debug.h"
#include "esp32.h"
#include "ff.h"

extern const uint8_t setup_htm_start[] asm("_binary_setup_htm_start");
extern const uint8_t setup_htm_end[] asm("_binary_setup_htm_end");

void page_default_install(void)
{
    FILINFO fi;
    if (f_stat("/cfw/setup.htm", &fi) == FR_OK)
    {
        return;
    }
    FIL f;
    UINT w = 0, n = (UINT)(setup_htm_end - setup_htm_start);
    f_mkdir("/cfw");
    if (f_open(&f, "/cfw/setup.tmp", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return;
    }
    FRESULT e = f_write(&f, setup_htm_start, n, &w);
    if ((f_close(&f) | e) != FR_OK || w != n || f_rename("/cfw/setup.tmp", "/cfw/setup.htm") != FR_OK)
    {
        f_unlink("/cfw/setup.tmp");
        debug_printf("page: built-in page NOT written\n");
        return;
    }
    debug_printf("page: built-in page written to the card (%u bytes)\n", n);
}
