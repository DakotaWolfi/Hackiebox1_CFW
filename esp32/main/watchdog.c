/*
 * Watchdog: the task watchdog
 * with a user entry that only the monitor (activity 1) and the panic feed;
 * its time-out is about 54 s as on the CC3200 (sdkconfig.defaults). At the
 * expiry the watchdog record is stored and the system's panic
 * handler resets the chip; the next start reports it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include "esp_task_wdt.h"
#include "buildblock.h"
#include "monitor.h"
#include "panic.h"
#include "panic_port.h"
#include "watchdog.h"

static esp_task_wdt_user_handle_t user;

bool watchdog_setup(void)
{
    return esp_task_wdt_add_user("cfw", &user) == ESP_OK;
}

void watchdog_feed(void)
{
    if (user)
    {
        esp_task_wdt_reset_user(user);
    }
}

void watchdog_handler(void)
{
    fault_watchdog();
}

/* Called by the task watchdog's interrupt before the panic. */
void esp_task_wdt_isr_user_handler(void)
{
    if (fault_claim())
    {
        port_retained_write(KIND_WATCHDOG | (uint32_t)monitor_op_position() << 4 | (buildblock_slot() & 3u) << 24,
                            REC_VERSION);
    }
}
