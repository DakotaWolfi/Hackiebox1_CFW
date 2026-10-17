/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "driverlib/udma.h"
#include "dma.h"
#include "lowram.h"

/* 32 channels, primary and alternate structures: 1024 bytes, aligned. */
static tDMAControlTable table[64] __attribute__((aligned(1024))) LOWRAM_DMA;

void udma_init(void)
{
    static bool done;

    if (done)
    {
        return;
    }
    done = true;
    MAP_PRCMPeripheralClkEnable(PRCM_UDMA, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralReset(PRCM_UDMA);
    MAP_uDMAEnable();
    MAP_uDMAControlBaseSet(table);
}
