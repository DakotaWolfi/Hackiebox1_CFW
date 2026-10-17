/*
 * Accelerometer identification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ACCEL_H
#define ACCEL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ACCEL_NONE = 0,
    ACCEL_MMA8451Q,
    ACCEL_LIS3DH,
} accel_type_t;

accel_type_t accel_identify(void);
const char *accel_name(accel_type_t t);

/* Configuration for the identified sensor. */
bool accel_configure(accel_type_t t);

/* True once accel_configure succeeded; readings before that fail. */
bool accel_active(void);
accel_type_t accel_sensor(void);

/* After a bus recovery: identify the sensor again and configure it; a
 * part identified earlier is kept when none answers. */
void accel_restore(void);

/* sensor read, then the mean of the stored samples. */
bool accel_orientation(int16_t xyz[3]);

typedef enum
{
    DIR_NONE = 0,
    DIR_A,
    DIR_B
} accel_dir_t;

accel_dir_t accel_tilt(const int16_t xyz[3]);

/* Slap detection, called when the interrupt line went high. */
accel_dir_t accel_slap(void);

#endif
