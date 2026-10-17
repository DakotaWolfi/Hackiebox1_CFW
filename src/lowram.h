/*
 * The SRAM below the image (0x20000000 to the handover record at
 * 0x20003FE0) is used by the ROM only while booting; the image uses it for
 * buffers that need no initial value (ld/app.ld, section .lowram, not
 * cleared at start). The handover record is read before anything is
 * placed there.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LOWRAM_H
#define LOWRAM_H

#ifdef ESP_PLATFORM
/* The ESP32 has no such region: ordinary bss. */
#define LOWRAM
#else
#define LOWRAM __attribute__((section(".lowram")))
#endif
/* The uDMA control table: first, for its 1024-byte alignment. */
#define LOWRAM_DMA __attribute__((section(".lowram.dma")))

#endif
