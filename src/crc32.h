/*
 * CRC-32 of the settings files: polynomial 0x04C11DB7, initial value 0, not
 * reflected, no final XOR (the Ogg page CRC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_ogg(uint32_t crc, const void *data, size_t len);

#endif
