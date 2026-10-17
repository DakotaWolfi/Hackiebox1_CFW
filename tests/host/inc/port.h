/*
 * Platform hooks (src/cc3200/port.h) for the host tests. The tests take
 * the CC3200's sock_port.h and flashfs.c with their SimpleLink stubs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PORT_H
#define PORT_H

#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#define TASK_STACK(words) (words)
#define TASK_STACK_FREE_WORDS(task) uxTaskGetStackHighWaterMark(task)
#define CRITICAL_ENTER() taskENTER_CRITICAL()
#define CRITICAL_EXIT() taskEXIT_CRITICAL()
#define START_HIB_EXIT 7
#define FLASHFS_MISSING (-14)

/* certificates from the card into the flash. */
#define PORT_CERT_INPUT 1
#define PORT_LANGUAGE_KEY 0

static inline void __attribute__((noreturn)) port_restart(void)
{
    printf("RESTART\n");
    exit(0);
}

#endif
