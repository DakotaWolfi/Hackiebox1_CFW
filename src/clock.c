/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "clock.h"
#include "tick.h"

static volatile uint32_t base; /* Unix seconds at tick 0 */

bool clock_set(uint32_t unix)
{
    uint32_t up = tick_ms() / 1000;
    if (unix <= up)
    {
        return false;
    }
    base = unix - up;
    return true;
}

bool clock_valid(uint32_t unix)
{
    return unix > tick_ms() / 1000;
}

uint32_t clock_now(void)
{
    return base + tick_ms() / 1000;
}

/* Days since 1970-01-01 of a proleptic Gregorian date and back. */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void clock_to_civil(uint32_t unix, civil_t *c)
{
    int32_t z = (int32_t)(unix / 86400) + 719468;
    uint32_t s = unix % 86400;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = (uint32_t)((int32_t)yoe + era * 400) + (c->month <= 2);
    c->hour = s / 3600;
    c->minute = s / 60 % 60;
    c->second = s % 60;
}

uint32_t clock_from_civil(const civil_t *c)
{
    int32_t days = days_from_civil((int32_t)c->year, c->month, c->day);
    if (days < 0)
    {
        return 0;
    }
    return (uint32_t)days * 86400 + c->hour * 3600 + c->minute * 60 + c->second;
}
