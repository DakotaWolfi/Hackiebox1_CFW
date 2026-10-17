/*
 * Platform hooks of the shared code, CC3200 version. Each platform has its
 * own port.h (src/cc3200/, esp32/main/) with the same names; here they are
 * macros and inline functions, so that the shared code compiles to what it
 * was before the split.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PORT_H
#define PORT_H

#include <stdbool.h>
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "FreeRTOS.h"
#include "task.h"

/* Stack of a static task, given in 32-bit words of this platform's code:
 * the array length and the depth passed to xTaskCreateStatic(). */
#define TASK_STACK(words) (words)
/* The least free stack a task has had, in 32-bit words. */
#define TASK_STACK_FREE_WORDS(task) uxTaskGetStackHighWaterMark(task)

/* A short critical section between activities (not from an interrupt). */
#define CRITICAL_ENTER() taskENTER_CRITICAL()
#define CRITICAL_EXIT() taskEXIT_CRITICAL()

/* Start reason of a hibernation exit (start_reason()). */
#define START_HIB_EXIT PRCM_HIB_EXIT

/* flashfs.h: the file does not exist (SL_FS_ERR_FILE_NOT_FOUND). */
#define FLASHFS_MISSING (-14)

/* Code and constant data used from the tick interrupt: all in RAM here. */
#define PORT_ISR_CODE
#define PORT_ISR_DATA

/* certificates from the card into the flash. */
#define PORT_CERT_INPUT 1

/* The box language in /settings.cfg, not in a key of its
 * own (unlike the ESP32). */
#define PORT_LANGUAGE_KEY 0

/* Reset of the application processor with its peripherals. */
static inline void __attribute__((noreturn)) port_restart(void)
{
    MAP_PRCMMCUReset(true);
    for (;;)
        ;
}

#endif
