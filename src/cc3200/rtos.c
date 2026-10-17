/*
 * Kernel hooks: a failed kernel assertion and a task stack overflow are
 * panics. The panic writes without the debug
 * lock in a handler or critical section, so a failure inside the kernel
 * cannot recurse.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FreeRTOS.h"
#include "task.h"
#include "panic.h"

/* The panic value holds the code position of the failed assertion in bits
 * 16-31 and its source line in bits 0-15. */
void rtos_assert_failed(int line)
{
    uint32_t pos = code_position((uint32_t)__builtin_return_address(0));
    panic(PANIC_ASSERT, pos << 16 | ((uint32_t)line & 0xFFFF), "kernel assertion");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    panic(PANIC_STACK_OVERFLOW, 0, name);
}
