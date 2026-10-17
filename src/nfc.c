/*
 * NFC reader TRF7970A and the ISO/IEC 15693 exchanges.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "board.h"
#include "debug.h"
#include "nfc.h"
#include "nfc_port.h"
#include "tick.h"

/* Registers and direct commands of the TRF7970A (datasheet). */
enum
{
    REG_STATUS = 0x00,
    REG_ISO = 0x01,
    REG_TX_PULSE = 0x06,
    REG_RX_NO_RESPONSE = 0x07,
    REG_RX_WAIT = 0x08,
    REG_MODULATOR = 0x09,
    REG_RX_SPECIAL = 0x0A,
    REG_IRQ = 0x0C,
    REG_IRQ_MASK = 0x0D,
    REG_RSSI = 0x0F,
    REG_SPECIAL = 0x10,
    REG_FIFO_STATUS = 0x1C,
    REG_TX_LENGTH = 0x1D,
    REG_FIFO = 0x1F,

    CMD_IDLE = 0x00,
    CMD_SOFT_INIT = 0x03,
    CMD_RESET_FIFO = 0x0F,
    CMD_TRANSMIT_NO_CRC = 0x10,
    CMD_BLOCK_RECEIVER = 0x16,

    ADDR_COMMAND = 0x80,
    ADDR_READ = 0x40,
    ADDR_CONTINUOUS = 0x20,

    IRQ_TX_END = 0x80,
    IRQ_RX = 0x40,
    IRQ_FIFO = 0x20,
    IRQ_CRC = 0x10,
    IRQ_PARITY = 0x08,
    IRQ_FRAMING = 0x04,
    IRQ_COLLISION = 0x02,
    IRQ_NO_RESPONSE = 0x01,
    IRQ_ERRORS = 0x1F,
};

#define FIFO_SIZE 12u

#define WAIT_STEP_MS 10u
#define RX_MAX 16u

static bool field_on;
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;
static volatile bool reinit_pending;
static unsigned spi_timeouts;

/* Diagnostics: IRQ status values of the last attempt. */
static uint8_t diag_irq[8];
static unsigned diag_n;
static uint8_t rx[RX_MAX];
static unsigned rx_len;

/* --- SPI framing --- */

void nfc_spi_init(void)
{
    nfc_port_init();
    lock = xSemaphoreCreateMutexStatic(&lock_mem);
    board_nfc_irq_enable();
}

void nfc_lock(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
}

void nfc_unlock(void)
{
    xSemaphoreGive(lock);
}

void nfc_reinit_later(void)
{
    reinit_pending = true;
}

bool nfc_reinit_pending(void)
{
    bool p = reinit_pending;
    reinit_pending = false;
    return p;
}

/* One byte each way; a controller that does not complete the word is
 * counted instead of hanging the task (0xFF read). */
static uint8_t spi_byte(uint8_t out)
{
    uint8_t in;
    if (!nfc_port_byte(out, &in))
    {
        spi_timeouts++;
        return 0xFF;
    }
    return in;
}

static void frame_begin(void)
{
    board_nfc_select(true);
}

static void frame_end(void)
{
    board_nfc_select(false);
    nfc_port_phase(0);
}

/* Bytes read are clocked with phase 1 while 0xFF is sent. */
static void frame_read(uint8_t *buf, unsigned n)
{
    nfc_port_phase(1);
    for (unsigned i = 0; i < n; i++)
    {
        buf[i] = spi_byte(0xFF);
    }
}

static void write_reg(uint8_t reg, uint8_t value)
{
    frame_begin();
    spi_byte(reg);
    spi_byte(value);
    frame_end();
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t v;
    frame_begin();
    spi_byte(ADDR_READ | reg);
    frame_read(&v, 1);
    frame_end();
    return v;
}

/* Stand-alone direct command with its extra byte where the reader needs
 * one. */
static void command(uint8_t cmd)
{
    frame_begin();
    spi_byte(ADDR_COMMAND | cmd);
    if (cmd != CMD_TRANSMIT_NO_CRC)
    {
        spi_byte(0x00);
    }
    frame_end();
}

/* The interrupt status is read together with the following register; the
 * read clears it. */
static uint8_t read_irq(void)
{
    uint8_t v[2];
    frame_begin();
    spi_byte(ADDR_READ | ADDR_CONTINUOUS | REG_IRQ);
    frame_read(v, 2);
    frame_end();
    return v[0];
}

/* --- reader set-up --- */

void nfc_reinit(void)
{
    tick_wait(5);
    command(CMD_SOFT_INIT);
    command(CMD_IDLE);
    tick_wait(5);
    field_on = false;
}

bool nfc_selftest(void)
{
    uint8_t status = read_reg(REG_STATUS), iso = read_reg(REG_ISO);
    write_reg(REG_IRQ_MASK, 0x3E);
    uint8_t mask = read_reg(REG_IRQ_MASK);
    write_reg(REG_IRQ_MASK, 0x00);
    uint8_t mask0 = read_reg(REG_IRQ_MASK);
    debug_verbose("nfc: status 0x%02x, ISO 0x%02x, mask write 0x3E read 0x%02x, 0x00 read 0x%02x, IRQ pin %d, "
                  "SPI timeouts %u\n",
                  status, iso, mask, mask0, board_nfc_irq(), spi_timeouts);
    return mask == 0x3E && mask0 == 0x00;
}

void nfc_field_on(uint8_t setting)
{
    if (field_on)
    {
        debug_printf("nfc: internal error, field already on\n");
    }
    write_reg(REG_STATUS, 0x21);
    write_reg(REG_ISO, 0x82);
    write_reg(REG_IRQ_MASK, 0x3E);
    write_reg(REG_MODULATOR, 0x21);
    write_reg(REG_TX_PULSE, 0x80);
    write_reg(REG_RX_NO_RESPONSE, 0x14);
    write_reg(REG_RX_WAIT, 0x1F);
    write_reg(REG_RX_SPECIAL, (uint8_t)(setting << 2));
    write_reg(REG_SPECIAL, read_reg(REG_SPECIAL) | 0x10);
    field_on = true;
    tick_wait(6);
}

void nfc_field_off(void)
{
    if (!field_on)
    {
        debug_printf("nfc: internal error, field already off\n");
    }
    write_reg(REG_STATUS, read_reg(REG_STATUS) & 0x1F);
    command(CMD_BLOCK_RECEIVER);
    field_on = false;
    tick_wait(6);
}

uint8_t nfc_rssi(void)
{
    return read_reg(REG_RSSI);
}

/* --- frames and CRC --- */

static uint16_t crc_step(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
    {
        crc = (crc & 1) ? (crc >> 1) ^ 0x8408u : crc >> 1;
    }
    return crc;
}

/* Result of an IRQ status with error bits. */
static nfc_result_t error_result(uint8_t irq)
{
    if (irq & IRQ_COLLISION)
    {
        return NFC_COLLISION;
    }
    if (irq & IRQ_NO_RESPONSE)
    {
        return NFC_NO_RESPONSE;
    }
    return NFC_RX_ERROR;
}

static uint8_t status_read(void)
{
    uint8_t irq = read_irq();
    if (diag_n < sizeof diag_irq)
    {
        diag_irq[diag_n++] = irq;
    }
    return irq;
}

/* The task waiting for the reader; the pin interrupt wakes it
 * at once, so a FIFO refill is not late by a scheduler tick. */
static TaskHandle_t volatile waiter;

void nfc_irq_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (waiter)
    {
        vTaskNotifyGiveFromISR(waiter, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* Waits at most one 10 ms step for the interrupt request; false when none
 * came. */
static bool wait_irq(void)
{
    uint32_t start = tick_ms();
    waiter = xTaskGetCurrentTaskHandle();
    while (!board_nfc_irq())
    {
        uint32_t elapsed = tick_ms() - start;
        if (elapsed >= WAIT_STEP_MS)
        {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, WAIT_STEP_MS - elapsed);
    }
    return true;
}

/* A bounded loop below ran out: noted once per run (the IRQ line or the
 * status kept the reader busy). */
static nfc_result_t guard_hit(unsigned step)
{
    static bool noted;
    if (!noted)
    {
        debug_printf("nfc: bound %u\n", step);
    }
    noted = true;
    return NFC_RX_ERROR;
}

/* One attempt: send the frame with its CRC, collect n_exp
 * bytes on the air, check them. */
static nfc_result_t attempt(const uint8_t *req, unsigned len, unsigned n_exp)
{
    uint8_t frame[24];
    uint16_t crc = 0xFFFF;
    memcpy(frame, req, len);
    for (unsigned i = 0; i < len; i++)
    {
        crc = crc_step(crc, req[i]);
    }
    crc = (uint16_t)~crc;
    frame[len] = (uint8_t)crc;
    frame[len + 1] = (uint8_t)(crc >> 8);
    unsigned n = len + 2;
    if (n_exp > RX_MAX)
    {
        n_exp = RX_MAX;
    }

    diag_n = 0;
    rx_len = 0;

    /* Step 1: clear an interrupt left pending. */
    read_irq();

    /* Step 2: Reset FIFO, Transmission without CRC, length in nibbles, the
     * first bytes into the 12-byte FIFO, in one chip-select frame. */
    unsigned sent = n < FIFO_SIZE ? n : FIFO_SIZE;
    frame_begin();
    spi_byte(ADDR_COMMAND | CMD_RESET_FIFO);
    spi_byte(ADDR_COMMAND | CMD_TRANSMIT_NO_CRC);
    spi_byte(ADDR_CONTINUOUS | REG_TX_LENGTH);
    spi_byte((uint8_t)(n >> 4));
    spi_byte((uint8_t)((n & 0x0F) << 4));
    for (unsigned i = 0; i < sent; i++)
    {
        spi_byte(frame[i]);
    }
    frame_end();

    /* Step 3: refill while bytes remain. */
    while (sent < n)
    {
        if (!wait_irq())
        {
            return NFC_RX_ERROR;
        }
        uint8_t irq = status_read();
        if (irq & IRQ_ERRORS)
        {
            return error_result(irq);
        }
        if ((irq & (IRQ_TX_END | IRQ_FIFO)) != (IRQ_TX_END | IRQ_FIFO))
        {
            return NFC_RX_ERROR;
        }
        unsigned chunk = n - sent < 8 ? n - sent : 8;
        for (unsigned i = 0; i < chunk; i++)
        {
            write_reg(ADDR_CONTINUOUS | REG_FIFO, frame[sent++]); /* 0x3F */
        }
    }

    /* Step 4: wait for RX; TX alone (end of transmission) is passed over. */
    /* The status reads are bounded: an interrupt line that stays high
     * must not keep this activity spinning. */
    uint8_t irq;
    for (unsigned guard = 0;; guard++)
    {
        if (guard >= 8)
        {
            return guard_hit(4);
        }
        if (!wait_irq())
        {
            return NFC_NO_RESPONSE;
        }
        irq = status_read();
        if (irq & IRQ_ERRORS)
        {
            return error_result(irq);
        }
        if (irq & IRQ_RX)
        {
            break;
        }
    }

    /* Step 5: collect n_exp bytes in chunks of at most 8. */
    for (unsigned guard = 0;; guard++)
    {
        if (guard >= 8)
        {
            return guard_hit(5);
        }
        if (!((irq & IRQ_RX) && !(irq & IRQ_FIFO)))
        {
            unsigned chunk = n_exp - rx_len < 8 ? n_exp - rx_len : 8;
            frame_begin();
            spi_byte(ADDR_READ | ADDR_CONTINUOUS | REG_FIFO);
            frame_read(rx + rx_len, chunk);
            frame_end();
            rx_len += chunk;
        }
        if (rx_len >= n_exp)
        {
            break;
        }
        if (!board_nfc_irq())
        {
            wait_irq();
        }
        irq = status_read();
        if (irq & IRQ_ERRORS)
        {
            return error_result(irq);
        }
    }

    /* the last position from the third byte on where the
     * running value is 0xF0B8 ends the frame. */
    if (rx_len < 3)
    {
        return NFC_SHORT;
    }
    crc = 0xFFFF;
    unsigned end = 0;
    for (unsigned i = 0; i < rx_len; i++)
    {
        crc = crc_step(crc, rx[i]);
        if (i >= 2 && crc == 0xF0B8u)
        {
            end = i + 1;
        }
    }
    if (!end)
    {
        return NFC_RX_ERROR;
    }
    rx_len = end - 2;
    return (rx[0] & 1) ? NFC_ERROR_FLAG : NFC_OK;
}

/* 2 and 4 are retried until three attempts are used, 6 ends
 * after min(retries + 1, 3) attempts. */
static nfc_result_t exchange(const uint8_t *req, unsigned len, unsigned retries, unsigned n_exp)
{
    unsigned collision_limit = retries + 1 < 3 ? retries + 1 : 3;
    nfc_result_t r = NFC_NO_RESPONSE;
    for (unsigned n = 1; n <= 3; n++)
    {
        r = attempt(req, len, n_exp);
        if (r == NFC_RX_ERROR || r == NFC_SHORT)
        {
            continue;
        }
        if (r == NFC_COLLISION && n < collision_limit)
        {
            continue;
        }
        break;
    }
    return r;
}

static nfc_result_t expect(nfc_result_t r, unsigned want, unsigned record)
{
    if (r == NFC_OK && rx_len != want)
    {
        debug_printf("nfc: record %u, received %u expected %u\n", record, rx_len, want);
        return NFC_SHORT;
    }
    return r;
}

unsigned nfc_last_length(void)
{
    return rx_len;
}

/* Builds flags, command, 0x04 and the UID when addressed. */
static unsigned header(uint8_t *f, uint8_t cmd, const uint8_t *uid)
{
    f[0] = uid ? 0x22 : 0x02;
    f[1] = cmd;
    f[2] = 0x04;
    if (!uid)
    {
        return 3;
    }
    memcpy(f + 3, uid, 8);
    return 11;
}

nfc_result_t nfc_random(const uint8_t *uid, unsigned retries, uint16_t *random)
{
    uint8_t f[16];
    unsigned n = header(f, 0xB2, uid);
    nfc_result_t r = expect(exchange(f, n, retries, 3 + 2), 3, 1725);
    if (r == NFC_OK)
    {
        *random = (uint16_t)(rx[1] | rx[2] << 8);
        tick_wait(6);
    }
    return r;
}

nfc_result_t nfc_inventory(uint8_t uid[8])
{
    static const uint8_t f[3] = {0x26, 0x01, 0x00};
    nfc_result_t r = expect(exchange(f, 3, 3, 10 + 2), 10, 1720);
    if (r == NFC_OK)
    {
        memcpy(uid, rx + 2, 8);
    }
    return r;
}

static unsigned put_key(uint8_t *f, uint16_t random, uint32_t password)
{
    uint32_t key = ((uint32_t)random | (uint32_t)random << 16) ^ password;
    for (int i = 0; i < 4; i++)
    {
        f[i] = (uint8_t)(key >> (8 * i));
    }
    return 4;
}

nfc_result_t nfc_set_password(const uint8_t *uid, uint16_t random, uint32_t password)
{
    uint8_t f[24];
    unsigned n = header(f, 0xB3, uid);
    f[n++] = 0x04;
    n += put_key(f + n, random, password);
    nfc_result_t r = exchange(f, n, 3, 1 + 2);
    if (r == NFC_NO_RESPONSE || r == NFC_ERROR_FLAG)
    {
        r = NFC_PASSWORD;
    }
    r = expect(r, 1, 1729);
    tick_wait(6);
    return r;
}

nfc_result_t nfc_read_block(const uint8_t uid[8], uint8_t block, uint8_t data[4])
{
    uint8_t f[16];
    unsigned n = header(f, 0x20, uid);
    /* READ SINGLE BLOCK carries no manufacturer code: the UID follows the
     * command byte. */
    memmove(f + 2, f + 3, 8);
    n = 10;
    f[n++] = block;
    tick_wait(6);
    nfc_result_t r = expect(exchange(f, n, 3, 5 + 2), 5, 1721);
    if (r == NFC_OK)
    {
        memcpy(data, rx + 1, 4);
    }
    tick_wait(6);
    return r;
}

/* commented out together with its call in tag.c */
// nfc_result_t nfc_enable_privacy(const uint8_t uid[8], uint16_t random, uint32_t password)
// {
//     uint8_t f[24];
//     unsigned n = header(f, 0xBA, uid);
//     n += put_key(f + n, random, password);
//     return expect(exchange(f, n, 3, 1 + 2), 1, 1730);
// }

/* --- diagnostics --- */

unsigned nfc_diag_irqs(const uint8_t **irqs)
{
    *irqs = diag_irq;
    return diag_n;
}
