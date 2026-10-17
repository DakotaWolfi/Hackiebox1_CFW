/*
 * NFC reader TRF7970A on the general-purpose SPI
 * and the ISO/IEC 15693 exchanges with an ICODE SLIX-L.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NFC_H
#define NFC_H

#include <stdbool.h>
#include <stdint.h>

/* Exchange result codes, plus 5 of the unlock. */
typedef enum
{
    NFC_OK = 0,
    NFC_NO_RESPONSE = 1,
    NFC_RX_ERROR = 2,
    NFC_ERROR_FLAG = 3,
    NFC_SHORT = 4,
    NFC_PASSWORD = 5,
    NFC_COLLISION = 6,
} nfc_result_t;

#define NFC_PASSWORD_BOXINE 0x7FFD6E5Bu
#define NFC_PASSWORD_FACTORY 0x0F0F0F0Fu

/* Start-up step 5: pins, 2 MHz, mode 0. */
void nfc_spi_init(void);

/* From the pin 18 interrupt. */
void nfc_irq_from_isr(void);

/* Reader access is blocked during a bus recovery:
 * the polling holds the lock for each round. */
void nfc_lock(void);
void nfc_unlock(void);

/* The recovery power-cycled the reader: the polling initialises it again
 * before its next access. */
void nfc_reinit_later(void);
bool nfc_reinit_pending(void);

/* Software Initialisation and Idle, 5 ms around. */
void nfc_reinit(void);

/* Diagnostic: register write/read-back over the SPI. */
bool nfc_selftest(void);

/* Field on with a receiver setting, field off. */
void nfc_field_on(uint8_t setting);
void nfc_field_off(void);

/* RSSI register 0x0F: main level bits 2:0, auxiliary level bits 5:3. */
uint8_t nfc_rssi(void);

/* Air commands. uid is in air order (B0 first) or NULL for unaddressed. */
nfc_result_t nfc_random(const uint8_t *uid, unsigned retries, uint16_t *random);
nfc_result_t nfc_inventory(uint8_t uid[8]);
nfc_result_t nfc_set_password(const uint8_t *uid, uint16_t random, uint32_t password);
nfc_result_t nfc_read_block(const uint8_t uid[8], uint8_t block, uint8_t data[4]);
// nfc_result_t nfc_enable_privacy(const uint8_t uid[8], uint16_t random, uint32_t password);

/* Diagnostics. */
/* IRQ status values read in the last attempt after its start. */
unsigned nfc_diag_irqs(const uint8_t **irqs);

/* Received data bytes of the last exchange (after the CRC check). */
unsigned nfc_last_length(void);

#endif
