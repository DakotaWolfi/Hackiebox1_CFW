/*
 * SPI of the NFC reader TRF7962A, ESP32-S3 version: SPI3 on SCLK 4, MOSI
 * 2, MISO 3, chip select GPIO1 by hand (board.c), IRQ on GPIO13
 * (teddiebox, reverse-engineering wiki). Implemented in nfc_port.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NFC_PORT_H
#define NFC_PORT_H

#include <stdbool.h>
#include <stdint.h>

void nfc_port_init(void);
/* Clock phase 0 (writing) or 1 (bytes read); polarity 0. */
void nfc_port_phase(unsigned phase);
bool nfc_port_byte(uint8_t out, uint8_t *in);

#endif
