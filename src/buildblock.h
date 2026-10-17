/*
 * Own build block.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUILDBLOCK_H
#define BUILDBLOCK_H

#include <stdint.h>

/* The ESP32 image carries one as well, for the server's
 * image library (esp32/main/buildid.c). */
typedef struct buildblock
{
    uint32_t magic1;
    uint32_t reserved0;
    uint32_t build_time;
    uint32_t debug;
    char revision[8];
    char build_date[61];
    uint8_t driverlib_version[3];
    uint8_t host_driver_version[4];
    uint8_t slot_binding;
    char family[4];
    uint8_t reserved1[3];
    uint32_t image_kind;
    uint32_t bootloader_word;
    uint32_t magic2;
} __attribute__((packed)) buildblock_t;

_Static_assert(sizeof(buildblock_t) == 112, "build block is 112 bytes");
/* The build block in SRAM, as the bootloader left it. */
const volatile buildblock_t *buildblock_in_sram(void);
/* Slot binding value, bits 0-2 of offset 92. */
uint8_t buildblock_slot(void);
/* At start, before the activities: panics on an invalid build block or a
 * platform number below 16; reports the slot binding value. */
void buildblock_check(void);

#endif
