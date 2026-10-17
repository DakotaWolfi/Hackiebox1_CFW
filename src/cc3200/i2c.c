/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/gpio.h"
#include "driverlib/i2c.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "debug.h"
#include "i2c.h"
#include "tick.h"

#define BASE I2CA0_BASE
#define SYSCLK 80000000u
#define TIMEOUT_COUNT 125u /* clock-low timeout: 2000 SCL periods */
#define WAIT_MS 500u

static bool fail_scl_high, fail_sda_high;

/* One bus, several activities: each register operation holds the bus. */
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;

static void bus_take(void)
{
    if (!lock)
    {
        lock = xSemaphoreCreateMutexStatic(&lock_mem);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void bus_give(void)
{
    xSemaphoreGive(lock);
}

void i2c_init(void)
{
    bus_take();
    MAP_PRCMPeripheralClkDisable(PRCM_I2CA0, PRCM_RUN_MODE_CLK);
    tick_wait(6);
    MAP_PRCMPeripheralClkEnable(PRCM_I2CA0, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralReset(PRCM_I2CA0);
    MAP_I2CMasterInitExpClk(BASE, SYSCLK, true); /* fast mode, 400 kHz */
    MAP_I2CTxFIFOFlush(BASE);
    MAP_I2CRxFIFOFlush(BASE);
    bus_give();
}

/* Line check after a supervised failure: pins 3 and
 * 4 as inputs for 10 ms, read, back to I2C mode. */
static void line_check(void)
{
    PinConfigSet(PIN_03, PIN_STRENGTH_2MA, PIN_TYPE_STD);
    PinConfigSet(PIN_04, PIN_STRENGTH_2MA, PIN_TYPE_STD);
    PinModeSet(PIN_03, PIN_MODE_0);
    PinModeSet(PIN_04, PIN_MODE_0);
    MAP_GPIODirModeSet(GPIOA1_BASE, 0x30, GPIO_DIR_MODE_IN);
    tick_wait(10);
    uint32_t v = MAP_GPIOPinRead(GPIOA1_BASE, 0x30);
    fail_scl_high = (v & 0x10) != 0;
    fail_sda_high = (v & 0x20) != 0;
    MAP_PinTypeI2C(PIN_03, PIN_MODE_5);
    MAP_PinTypeI2C(PIN_04, PIN_MODE_5);
}

static i2c_result_t transact(uint8_t addr, uint32_t cmd)
{
    uint32_t start = tick_ms();

    MAP_I2CMasterIntClearEx(BASE, 0xFFFFFFFFu);
    MAP_I2CMasterTimeoutSet(BASE, TIMEOUT_COUNT);
    MAP_I2CMasterControl(BASE, cmd);
    for (;;)
    {
        uint32_t st = MAP_I2CMasterIntStatusEx(BASE, false);
        if (st & I2C_MASTER_INT_TIMEOUT || tick_ms() - start > WAIT_MS)
        {
            uint32_t waited = tick_ms() - start;
            line_check();
            debug_printf("i2c 0x%02x: no completion after %lu ms, SCL %d SDA %d\n", addr,
                         (unsigned long)waited, fail_scl_high, fail_sda_high);
            if (cmd != I2C_MASTER_CMD_SINGLE_RECEIVE)
            {
                MAP_I2CMasterControl(BASE, I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
            }
            return I2C_TIMEOUT;
        }
        if (st & I2C_MASTER_INT_DATA)
        {
            break;
        }
    }
    if (MAP_I2CMasterErr(BASE) != I2C_MASTER_ERR_NONE)
    {
        if (cmd != I2C_MASTER_CMD_SINGLE_RECEIVE)
        {
            MAP_I2CMasterControl(BASE, cmd == I2C_MASTER_CMD_BURST_RECEIVE_START ||
                                               cmd == I2C_MASTER_CMD_BURST_RECEIVE_CONT ||
                                               cmd == I2C_MASTER_CMD_BURST_RECEIVE_FINISH
                                           ? I2C_MASTER_CMD_BURST_RECEIVE_ERROR_STOP
                                           : I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
        }
        return I2C_NACK;
    }
    return I2C_OK;
}

static i2c_result_t write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    i2c_result_t r;

    MAP_I2CMasterSlaveAddrSet(BASE, addr, false);
    MAP_I2CMasterDataPut(BASE, reg);
    if ((r = transact(addr, I2C_MASTER_CMD_BURST_SEND_START)) != I2C_OK)
    {
        return r;
    }
    MAP_I2CMasterDataPut(BASE, value);
    return transact(addr, I2C_MASTER_CMD_BURST_SEND_FINISH);
}

static i2c_result_t read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len)
{
    i2c_result_t r;

    if (len == 0)
    {
        return I2C_OK;
    }
    /* START, address+W, register, then a repeated START (no STOP). */
    MAP_I2CMasterSlaveAddrSet(BASE, addr, false);
    MAP_I2CMasterDataPut(BASE, len > 1 ? reg | 0x80 : reg);
    if ((r = transact(addr, I2C_MASTER_CMD_BURST_SEND_START)) != I2C_OK)
    {
        return r;
    }
    MAP_I2CMasterSlaveAddrSet(BASE, addr, true);
    if (len == 1)
    {
        if ((r = transact(addr, I2C_MASTER_CMD_SINGLE_RECEIVE)) != I2C_OK)
        {
            return r;
        }
        buf[0] = (uint8_t)MAP_I2CMasterDataGet(BASE);
        return I2C_OK;
    }
    if ((r = transact(addr, I2C_MASTER_CMD_BURST_RECEIVE_START)) != I2C_OK)
    {
        return r;
    }
    for (uint8_t i = 0; i < len; i++)
    {
        buf[i] = (uint8_t)MAP_I2CMasterDataGet(BASE);
        if (i + 1 < len &&
            (r = transact(addr, i + 2 < len ? I2C_MASTER_CMD_BURST_RECEIVE_CONT
                                            : I2C_MASTER_CMD_BURST_RECEIVE_FINISH)) != I2C_OK)
        {
            return r;
        }
    }
    return I2C_OK;
}

i2c_result_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    bus_take();
    i2c_result_t r = write_reg(addr, reg, value);
    bus_give();
    return r;
}

i2c_result_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len)
{
    bus_take();
    i2c_result_t r = read_regs(addr, reg, buf, len);
    bus_give();
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
