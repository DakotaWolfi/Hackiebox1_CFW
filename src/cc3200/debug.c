/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/uart.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "debug.h"

#define SYSCLK 80000000u

static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;

void debug_init(void)
{
    MAP_PRCMPeripheralClkEnable(PRCM_UARTA0, PRCM_RUN_MODE_CLK);
    MAP_PinTypeUART(PIN_55, PIN_MODE_3);
    MAP_UARTConfigSetExpClk(UARTA0_BASE, SYSCLK, 921600,
                            UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    lock = xSemaphoreCreateMutexStatic(&lock_mem);
}

bool debug_task_context(void)
{
    uint32_t ipsr, primask, basepri;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("mrs %0, basepri" : "=r"(basepri));
    return (ipsr & 0x1FF) == 0 && !(primask & 1) && basepri == 0 &&
           xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/* Polled output, no lock. Nothing is sent while the UART has no clock
 * (before debug_init), so a fault that early cannot lock up the bus. */
void debug_write(const char *s, int n)
{
    if (!MAP_PRCMPeripheralStatusGet(PRCM_UARTA0))
    {
        return;
    }
    for (int i = 0; i < n && s[i]; i++)
    {
        if (s[i] == '\n')
        {
            MAP_UARTCharPut(UARTA0_BASE, '\r');
        }
        MAP_UARTCharPut(UARTA0_BASE, s[i]);
    }
}

#if CFW_LOG_UART
/* Raw bytes: the log frames copied to UART0; the lock only in
 * task context. */
void debug_write_bin(const void *p, unsigned n)
{
    const uint8_t *b = p;
    if (!MAP_PRCMPeripheralStatusGet(PRCM_UARTA0))
    {
        return;
    }
    bool locked = lock && debug_task_context() && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
    for (unsigned i = 0; i < n; i++)
    {
        MAP_UARTCharPut(UARTA0_BASE, b[i]);
    }
    if (locked)
    {
        xSemaphoreGive(lock);
    }
}
#endif

void debug_flush(void)
{
    if (MAP_PRCMPeripheralStatusGet(PRCM_UARTA0))
    {
        while (MAP_UARTBusy(UARTA0_BASE))
            ;
    }
}

/* The lock is only taken from a task with interrupts enabled and the
 * scheduler running; an interrupt handler, a critical section or a fault
 * handler writes without it (a fault while the lock is held would
 * otherwise recurse through the kernel's assertions). */
void debug_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
    {
        n = sizeof buf - 1;
    }
    bool locked = lock && debug_task_context() && xSemaphoreTake(lock, portMAX_DELAY);
    debug_write(buf, n);
    if (locked)
    {
        xSemaphoreGive(lock);
    }
}
