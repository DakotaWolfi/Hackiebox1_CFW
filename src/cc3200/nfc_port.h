/*
 * SPI of the NFC reader, CC3200 version: the general-purpose SPI on pins
 * 5 to 7 at 2 MHz, chip select by a GPIO (board.c).
 * Only nfc.c includes it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NFC_PORT_H
#define NFC_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/spi.h"

#define NFC_SPI_CLOCK 2000000u
#define NFC_SPI_FLAGS (SPI_SW_CTRL_CS | SPI_3PIN_MODE | SPI_TURBO_OFF | SPI_CS_ACTIVELOW | SPI_WL_8)

/* Clock phase 0 (writing) or 1 (bytes read); polarity 0. */
static inline void nfc_port_phase(unsigned phase)
{
    MAP_SPIDisable(GSPI_BASE);
    MAP_SPIConfigSetExpClk(GSPI_BASE, MAP_PRCMPeripheralClockGet(PRCM_GSPI), NFC_SPI_CLOCK, SPI_MODE_MASTER,
                           phase ? SPI_SUB_MODE_1 : SPI_SUB_MODE_0, NFC_SPI_FLAGS);
    MAP_SPIEnable(GSPI_BASE);
    /* The controller's own chip select is not on a pin (pin 8 is a GPIO);
     * it is forced active so that every word written is clocked out. */
    MAP_SPICSEnable(GSPI_BASE);
}

static inline void nfc_port_init(void)
{
    MAP_PRCMPeripheralClkEnable(PRCM_GSPI, PRCM_RUN_MODE_CLK);
    MAP_PinTypeSPI(PIN_05, PIN_MODE_7);
    MAP_PinTypeSPI(PIN_06, PIN_MODE_7);
    MAP_PinTypeSPI(PIN_07, PIN_MODE_7);
    MAP_PRCMPeripheralReset(PRCM_GSPI);
    MAP_SPIReset(GSPI_BASE);
    nfc_port_phase(0);
}

/* One byte each way; false when the controller does not complete the word
 * within about 1 ms. */
static inline bool nfc_port_byte(uint8_t out, uint8_t *in)
{
    unsigned long v = 0xFF;
    unsigned n = 0;
    while (!MAP_SPIDataPutNonBlocking(GSPI_BASE, out))
    {
        if (++n > 20000)
        {
            return false;
        }
    }
    n = 0;
    while (!MAP_SPIDataGetNonBlocking(GSPI_BASE, &v))
    {
        if (++n > 20000)
        {
            return false;
        }
    }
    *in = (uint8_t)v;
    return true;
}

#endif
