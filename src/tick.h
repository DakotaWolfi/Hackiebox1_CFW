/*
 * Millisecond counter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TICK_H
#define TICK_H

#include <stdint.h>

uint32_t tick_ms(void);
/* Waits at least n whole ticks; only from a task. */
void tick_wait(uint32_t n);

#endif
