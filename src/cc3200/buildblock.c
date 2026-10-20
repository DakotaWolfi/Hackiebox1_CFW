/*
 * Build block with the own identity and the
 * family marker CFW1. All values come from the build (Makefile).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "buildblock.h"
#include "debug.h"
#include "handover.h"
#include "panic.h"
#include "version.h"

__attribute__((section(".buildblock"), used))
const buildblock_t buildblock = {
    .magic1 = 0xBEAC0005u,
    .build_time = CFW_BUILD_TIME,
    .revision = CFW_REVISION,
    .build_date = CFW_BUILD_DATE,
    .driverlib_version = {1, 2, 0},
    .host_driver_version = {1, 0, 1, 14},
    .family = {'C', 'F', 'W', '1'},
    .image_kind = 2,
    .magic2 = 0xBEAC0005u,
};

const volatile buildblock_t *buildblock_in_sram(void)
{
    return &buildblock;
}

uint8_t buildblock_slot(void)
{
    return buildblock_in_sram()->slot_binding & 7;
}

/* both magics and a slot binding value of 3 or less. */
static bool valid(void)
{
    const volatile buildblock_t *b = buildblock_in_sram();
    return b->magic1 == 0xBEAC0005u && b->magic2 == 0xBEAC0005u && (b->slot_binding & 7) <= 3;
}

/* The start banner line "boot:"
 * gives the inputs of the decisions before a panic can follow: handover
 * record (v valid, X magic but invalid, - none), its slot, kind and word,
 * the build block's own word and slot binding value (7489 for a value
 * other than 2 and 3). */
void buildblock_check(void)
{
    const handover_t *h = handover_get();
    const volatile buildblock_t *b = buildblock_in_sram();
    uint32_t w = b->bootloader_word;
    debug_printf("boot: ho %c %u/%u w %08lx bb %08lx slot %u\n", h->valid ? 'v' : h->magic ? 'X'
                                                                                           : '-',
                 h->slot,
                 h->kind, (unsigned long)h->bootloader_word, (unsigned long)w, b->slot_binding);
    if (!valid())
    {
        panic(PANIC_BUILDBLOCK, 0, "own build block invalid");
    }
    if (!w && h->valid)
    {
        w = h->bootloader_word;
    }
    /* Under the SD bootloader a missing word counts as platform 16 until
     * /sys/pre-img.bin has been read (cloud.c). */
    if (!w && h->valid)
    {
        return;
    }
    unsigned platform = w >> 16;
    if (platform < 16)
    {
        panic(PANIC_PLATFORM, w, "hardware platform not supported");
    }
    if (platform > 17)
    {
        debug_verbose("boot: platform %u\n", platform);
    }
}
