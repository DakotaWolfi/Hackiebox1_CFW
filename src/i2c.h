/*
 * I2C master on I2C0.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef I2C_H
#define I2C_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    I2C_OK = 0,
    I2C_NACK,    /* the controller ended the operation with an error */
    I2C_TIMEOUT, /* clock-low timeout or 500 ms passed (supervised failure) */
} i2c_result_t;

/* Clock off, 6 ms, clock on, reset, master at 400 kHz, FIFOs flushed. */
void i2c_init(void);

i2c_result_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value);
/* Reads len bytes; for len > 1 bit 7 of the register byte is set. */
i2c_result_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);

/* Levels of SCL and SDA when the last supervised failure happened. */
bool i2c_failed_scl_high(void);
bool i2c_failed_sda_high(void);

#endif
