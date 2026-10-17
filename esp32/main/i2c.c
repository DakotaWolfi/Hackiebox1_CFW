/*
 * I2C master on GPIO5 (SDA) and GPIO6 (SCL) at 400 kHz, for the codec and
 * the accelerometer. A transfer
 * that the controller does not finish within 500 ms is a supervised
 * failure (I2C_TIMEOUT), a missing acknowledge I2C_NACK.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "debug.h"
#include "i2c.h"

#define PIN_SDA GPIO_NUM_5
#define PIN_SCL GPIO_NUM_6
#define TIMEOUT_MS 500
#define DEVICES 4

static i2c_master_bus_handle_t bus;
static struct
{
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} devs[DEVICES];
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;
static bool fail_scl_high, fail_sda_high;

void i2c_init(void)
{
    if (!lock)
    {
        lock = xSemaphoreCreateMutexStatic(&lock_mem);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (bus)
    {
        for (int i = 0; i < DEVICES; i++)
        {
            if (devs[i].dev)
            {
                i2c_master_bus_rm_device(devs[i].dev);
            }
            devs[i].dev = NULL;
            devs[i].addr = 0;
        }
        i2c_del_master_bus(bus);
        bus = NULL;
    }
    i2c_master_bus_config_t c = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    esp_err_t e = i2c_new_master_bus(&c, &bus);
    if (e != ESP_OK)
    {
        debug_printf("i2c: bus %d\n", e);
        bus = NULL;
    }
    xSemaphoreGive(lock);
}

static i2c_master_dev_handle_t device(uint8_t addr)
{
    int free = -1;
    for (int i = 0; i < DEVICES; i++)
    {
        if (devs[i].dev && devs[i].addr == addr)
        {
            return devs[i].dev;
        }
        if (!devs[i].dev && free < 0)
        {
            free = i;
        }
    }
    if (free < 0 || !bus)
    {
        return NULL;
    }
    i2c_device_config_t d = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &d, &devs[free].dev) != ESP_OK)
    {
        devs[free].dev = NULL;
        return NULL;
    }
    devs[free].addr = addr;
    return devs[free].dev;
}

static i2c_result_t result(esp_err_t e)
{
    if (e == ESP_OK)
    {
        return I2C_OK;
    }
    if (e == ESP_ERR_TIMEOUT)
    {
        /* the line levels at the failure. */
        fail_scl_high = gpio_get_level(PIN_SCL);
        fail_sda_high = gpio_get_level(PIN_SDA);
        return I2C_TIMEOUT;
    }
    return I2C_NACK;
}

i2c_result_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    const uint8_t b[2] = {reg, value};
    xSemaphoreTake(lock, portMAX_DELAY);
    i2c_master_dev_handle_t d = device(addr);
    i2c_result_t r = d ? result(i2c_master_transmit(d, b, 2, TIMEOUT_MS)) : I2C_NACK;
    xSemaphoreGive(lock);
    return r;
}

i2c_result_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t r0 = len > 1 ? (uint8_t)(reg | 0x80) : reg;
    xSemaphoreTake(lock, portMAX_DELAY);
    i2c_master_dev_handle_t d = device(addr);
    i2c_result_t r = d ? result(i2c_master_transmit_receive(d, &r0, 1, buf, len, TIMEOUT_MS)) : I2C_NACK;
    xSemaphoreGive(lock);
    return r;
}

bool i2c_failed_scl_high(void)
{
    return fail_scl_high;
}

bool i2c_failed_sda_high(void)
{
    return fail_sda_high;
}
