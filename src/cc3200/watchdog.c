/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/interrupt.h"
#include "driverlib/prcm.h"
#include "driverlib/wdt.h"
#include "panic.h"
#include "watchdog.h"

static bool running;

bool watchdog_setup(void)
{
    running = PRCMPeripheralStatusGet(PRCM_WDT) && MAP_WatchdogRunning(WDT_BASE);
    if (!running)
    {
        return false;
    }
    MAP_WatchdogUnlock(WDT_BASE);
    MAP_WatchdogReloadSet(WDT_BASE, 0xFFFFFFFFu);
    MAP_WatchdogLock(WDT_BASE);
    watchdog_feed();
    /* the first expiry is served at the highest priority, above
     * the kernel's critical sections; it takes effect when the scheduler
     * enables the interrupts. */
    MAP_IntPrioritySet(INT_WDT, INT_PRIORITY_LVL_0);
    MAP_IntEnable(INT_WDT);
    return true;
}

void watchdog_feed(void)
{
    if (!running)
    {
        return;
    }
    MAP_WatchdogUnlock(WDT_BASE);
    MAP_WatchdogIntClear(WDT_BASE);
    MAP_WatchdogLock(WDT_BASE);
}

void watchdog_handler(void)
{
    fault_watchdog();
}
