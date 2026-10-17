/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "crc32.h"

uint32_t crc32_ogg(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len--)
    {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++)
        {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}
