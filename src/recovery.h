/*
 * I2C bus recovery.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RECOVERY_H
#define RECOVERY_H

/* Steps 1 to 8 and the accelerometer part of step 9; the codec part of
 * step 9 is done by the caller (codec.c, sound.c). Only from a task. */
void bus_recovery(void);

#endif
