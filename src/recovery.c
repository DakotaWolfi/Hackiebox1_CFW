/*
 * I2C bus recovery: power-cycle the peripheral supply with the
 * peripheral reset asserted, initialise the I2C controller again and
 * configure the accelerometer again.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "accel.h"
#include "board.h"
#include "debug.h"
#include "i2c.h"
#include "nfc.h"
#include "recovery.h"
#include "tick.h"

void bus_recovery(void)
{
    uint32_t t0 = tick_ms();
    nfc_lock();
    board_peripheral_reset(true);
    board_peripheral_supply(false);
    tick_wait(100);
    board_peripheral_supply(true);
    tick_wait(100);
    board_peripheral_reset(false);
    nfc_reinit_later();
    nfc_unlock();
    i2c_init();
    tick_wait(100);
    accel_restore();
    debug_printf("i2c: recovery %lu ms\n", (unsigned long)(tick_ms() - t0));
}
