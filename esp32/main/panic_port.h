/*
 * Hooks of the shared panic code (panic.c), ESP32-S3 version: the two
 * retained words live in RTC memory that a reset and deep sleep keep; the
 * reset reason is mapped to the CC3200 numbers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PANIC_PORT_H
#define PANIC_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define START_POWER_ON 0u
#define START_CORE_RESET 3u
#define START_MCU_RESET 4u
#define START_WDT_RESET 5u
#define START_BROWNOUT 6u

static inline uint32_t port_irq_save(void)
{
    return (uint32_t)portSET_INTERRUPT_MASK_FROM_ISR();
}

static inline void port_irq_restore(uint32_t pm)
{
    portCLEAR_INTERRUPT_MASK_FROM_ISR(pm);
}

static inline bool port_in_isr(void)
{
    return xPortInIsrContext();
}

unsigned port_reset_cause(void);
void port_retained_read(uint32_t *w1, uint32_t *w2);
void port_retained_write(uint32_t w1, uint32_t w2);

#endif
