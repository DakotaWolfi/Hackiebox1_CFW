/*
 * Wall clock: Unix seconds anchored against the millisecond tick, and the
 * conversion to the network processor's
 * date format.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Sets the time base so that now is unix; false (nothing changed) when
 * unix is not later than the uptime in whole seconds. */
bool clock_set(uint32_t unix);

/* True when clock_set(unix) would apply it now (later than the uptime). */
bool clock_valid(uint32_t unix);

/* Current Unix seconds. */
uint32_t clock_now(void);

typedef struct
{
    uint32_t year, month, day, hour, minute, second;
} civil_t;

void clock_to_civil(uint32_t unix, civil_t *c);
uint32_t clock_from_civil(const civil_t *c);

#endif
