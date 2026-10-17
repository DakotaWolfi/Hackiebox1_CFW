/*
 * Debug output on UART0. Developer output only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

void debug_init(void);
void debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* CFW_DEBUG (Makefile): 1 keeps the messages of normal operation, errors,
 * state changes and those standing for log records; 2 adds the verbose
 * bring-up diagnostics of debug_verbose(). At level 1 a debug_verbose()
 * call is compiled out: its arguments are checked but never evaluated, so
 * they must have no side effects. */
#ifndef CFW_DEBUG
#define CFW_DEBUG 1
#endif
/* CFW_VERBOSE (Makefile, module names such as "net cloud") enables
 * debug_verbose() for those modules only: level 2 for everything no longer
 * fits the SRAM. */
#if CFW_DEBUG >= 2 || defined(CFW_VERBOSE_MODULE)
#define debug_verbose(...) debug_printf(__VA_ARGS__)
#else
#define debug_verbose(...)             \
    do                                 \
    {                                  \
        if (0)                         \
            debug_printf(__VA_ARGS__); \
    } while (0)
#endif
/* Polled output without the lock (fault handlers); n characters at most. */
void debug_write(const char *s, int n);
/* Waits until the UART has sent everything. */
void debug_flush(void);
/* True in a task with interrupts enabled while the scheduler runs. */
bool debug_task_context(void);

#endif
