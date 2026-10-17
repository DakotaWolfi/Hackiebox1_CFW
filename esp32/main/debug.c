/*
 * Debug output on UART0 (J103 TxD) at 921600 Bd,
 * the rate of the box's bootloader, through the UART driver's transmit
 * buffer; a fault writes polled through the ROM. Developer output only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "driver/uart.h"
#include "esp_rom_uart.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "debug.h"

static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;
static bool driver;

void debug_init(void)
{
    lock = xSemaphoreCreateMutexStatic(&lock_mem);
    driver = uart_driver_install(UART_NUM_0, 256, 4096, 0, NULL, 0) == ESP_OK;
}

bool debug_task_context(void)
{
    return !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/* Polled output, no lock. */
void debug_write(const char *s, int n)
{
    for (int i = 0; i < n && s[i]; i++)
    {
        if (s[i] == '\n')
        {
            esp_rom_output_putc('\r');
        }
        esp_rom_output_putc(s[i]);
    }
}

void debug_flush(void)
{
    if (driver && debug_task_context())
    {
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(500));
    }
    esp_rom_output_tx_wait_idle(0);
}

/* In a task: formatted into static buffers under the lock, so that the
 * small stacks of the activities carry only the formatting itself. */
void debug_printf(const char *fmt, ...)
{
    static char buf[200], out[400];
    va_list ap;

    if (!driver || !debug_task_context())
    {
        char tmp[120];
        va_start(ap, fmt);
        int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
        va_end(ap);
        debug_write(tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
    {
        n = sizeof buf - 1;
    }
    /* \n -> \r\n for the terminal */
    int k = 0;
    for (int i = 0; i < n; i++)
    {
        if (buf[i] == '\n')
        {
            out[k++] = '\r';
        }
        out[k++] = buf[i];
    }
    if (k > 0)
    {
        uart_write_bytes(UART_NUM_0, out, (size_t)k);
    }
    xSemaphoreGive(lock);
}
