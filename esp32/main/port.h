/*
 * Platform hooks of the shared code, ESP32-S3 version (see
 * src/cc3200/port.h for the names). ESP-IDF's FreeRTOS counts stacks in
 * bytes, and Xtensa code with its register windows needs about twice the
 * stack of the Cortex-M4 code the sizes were measured on.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PORT_H
#define PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "FreeRTOS.h"
#include "task.h"

#define TASK_STACK(words) ((words) * 6u + 1024u)
#define TASK_STACK_FREE_WORDS(task) (uxTaskGetStackHighWaterMark(task) / 6u)

extern portMUX_TYPE port_mux;
#define CRITICAL_ENTER() taskENTER_CRITICAL(&port_mux)
#define CRITICAL_EXIT() taskEXIT_CRITICAL(&port_mux)

/* Code and constant data used from the tick interrupt (the tick hook in
 * input.c and what it calls): the SysTick interrupt also runs while a
 * flash operation has the cache disabled, so they must be in IRAM/DRAM. */
#define PORT_ISR_CODE IRAM_ATTR
#define PORT_ISR_DATA DRAM_ATTR

/* Start reasons as the CC3200 numbers them (panic_port.h): a wake-up from
 * deep sleep counts as the hibernation exit. */
#define START_HIB_EXIT 7u

/* flashfs.h: the file does not exist. */
#define FLASHFS_MISSING (-14)

/* certificates from the card into the flash. */
#define PORT_CERT_INPUT 0

/* the box language as the u8 key "lang" of the namespace cfw
 * instead of /settings.cfg; false when absent or not stored (nvsfile.c). */
#define PORT_LANGUAGE_KEY 1
bool port_language_read(uint8_t *l);
bool port_language_write(uint8_t l);

/* Peripheral rails off, then a software reset. */
void port_restart(void) __attribute__((noreturn));

#endif
