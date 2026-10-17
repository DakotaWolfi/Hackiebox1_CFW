/*
 * Watchdog.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>

/* Sets the reload value to 0xFFFFFFFF and enables the expiry interrupt;
 * returns whether the watchdog runs. */
bool watchdog_setup(void);
/* Only the monitor (activity 1) and the panic (step 3) feed the
 * watchdog; activity 1 uses monitor_feed() for the
 * special waits. */
void watchdog_feed(void);
/* Vector table entry (startup.c). */
void watchdog_handler(void);

#endif
