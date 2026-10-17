/*
 * Millisecond counter: the kernel tick at 1 kHz
 * (CONFIG_FREERTOS_HZ).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FreeRTOS.h"
#include "task.h"
#include "tick.h"

uint32_t tick_ms(void)
{
    return xPortInIsrContext() ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
}

void tick_wait(uint32_t n)
{
    vTaskDelay(n + 1);
}
