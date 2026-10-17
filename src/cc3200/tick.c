/*
 * The millisecond counter is the kernel tick count: the kernel runs the
 * SysTick with a period of 80 000 system-clock cycles, and the counter is 0
 * when the scheduler starts (start-up step 11).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FreeRTOS.h"
#include "task.h"
#include "tick.h"

uint32_t tick_ms(void)
{
    return xTaskGetTickCount();
}

void tick_wait(uint32_t n)
{
    vTaskDelay(n + 1);
}
