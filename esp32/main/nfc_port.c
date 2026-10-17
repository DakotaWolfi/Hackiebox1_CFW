/*
 * SPI of the NFC reader (nfc_port.h): SPI3 at 2 MHz, polarity 0, one
 * device per clock phase on the same lines; the chip select is a GPIO
 * driven by nfc.c through board.c, so the reader's SPI frames span several
 * bytes. One byte per polled transaction.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "debug.h"
#include "nfc_port.h"

#define CLOCK_HZ 2000000

static spi_device_handle_t dev[2];
static unsigned phase;

void nfc_port_init(void)
{
    spi_bus_config_t b = {
        .mosi_io_num = 2,
        .miso_io_num = 3,
        .sclk_io_num = 4,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    if (spi_bus_initialize(SPI3_HOST, &b, SPI_DMA_DISABLED) != ESP_OK)
    {
        debug_printf("nfc: SPI bus FAILED\n");
        return;
    }
    for (int m = 0; m < 2; m++)
    {
        spi_device_interface_config_t d = {
            .mode = m, /* CPOL 0, CPHA m */
            .clock_speed_hz = CLOCK_HZ,
            .spics_io_num = -1,
            .queue_size = 1,
        };
        if (spi_bus_add_device(SPI3_HOST, &d, &dev[m]) != ESP_OK)
        {
            debug_printf("nfc: SPI device FAILED\n");
        }
    }
    phase = 0;
}

void nfc_port_phase(unsigned p)
{
    phase = p ? 1 : 0;
}

bool nfc_port_byte(uint8_t out, uint8_t *in)
{
    if (!dev[phase])
    {
        return false;
    }
    spi_transaction_t t = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {out},
    };
    if (spi_device_polling_transmit(dev[phase], &t) != ESP_OK)
    {
        return false;
    }
    *in = t.rx_data[0];
    return true;
}
