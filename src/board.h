/*
 * Pins, supplies and the start-up order of the hardware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Start-up steps 1 to 10, before the system tick starts. */
void board_early_init(void);

/* Levels of the I2C lines at the start-up check. */
/* Pins 61/62 as left by the boot loader: mode 61 << 12 | mode 62 << 8 |
 * direction bits 7..6 | levels 61 << 0, 62 << 1 (diagnostic). */
uint32_t board_loader_pins(void);
bool board_scl_high_at_start(void);
bool board_sda_high_at_start(void);

bool board_on_station(void);
bool board_ear_big_pressed(void);
bool board_ear_small_pressed(void);

/* SD card supply (pin 58, low = on). */
void board_sd_supply(bool on);

/* Peripheral supply (pin 61) and reset (pin 62). */
void board_peripheral_supply(bool on);
void board_peripheral_reset(bool asserted);

/* NFC reader chip select (pin 16, low = selected) and interrupt request
 * (pin 18, high = request). */
void board_nfc_select(bool selected);
bool board_nfc_irq(void);
void board_nfc_irq_enable(void);

/* Accelerometer interrupt line (pin 15, high = event). */
bool board_accel_int(void);

#endif
