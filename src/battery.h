/*
 * Battery voltage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

/* One measurement in mV; the first call sets the ADC up. */
uint32_t battery_measure(void);
/* The value of the last measurement, 0 before the first (F12 status). */
uint32_t battery_last_mv(void);
/* The ADC word of the last measurement (start-up log). */
uint32_t battery_last_raw(void);
/* CFW_HIBERNATE only: one measurement right after a wake-up,
 * before the scheduler starts. */
uint32_t battery_wake_measure(void);

#endif
