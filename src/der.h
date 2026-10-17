/*
 * The size of a DER element with tag 0x30 (ITU-T X.690) from its header,
 * as the check of a certificate or key needs it: 0 when the first bytes
 * are not such a header (length up to 65535 bytes).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DER_H
#define DER_H

#include <stdint.h>

/* p holds n bytes (the first 4 are enough). */
static inline uint32_t der_element_size(const uint8_t *p, uint32_t n)
{
    if (n < 2 || p[0] != 0x30)
    {
        return 0;
    }
    if (p[1] < 0x80)
    {
        return 2u + p[1];
    }
    if (p[1] == 0x81 && n > 2)
    {
        return 3u + p[2];
    }
    if (p[1] == 0x82 && n > 3)
    {
        return 4u + ((uint32_t)p[2] << 8 | p[3]);
    }
    return 0;
}

#endif
