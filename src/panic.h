/*
 * Panic, fault records and the retained record.
 *
 * Without CFW_HIBERNATE (the default) the hibernate restart and the off
 * state are replaced by an MCU reset, and a panic, hard fault or watchdog
 * record found after a core, MCU or watchdog reset is reported as after a
 * hibernation exit, so that the fault report still works.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PANIC_H
#define PANIC_H

#include <stdbool.h>
#include <stdint.h>

#ifndef CFW_HIBERNATE
#define CFW_HIBERNATE 0
#endif

/* Activity numbers. */
typedef enum
{
    ACT_NONE = 0,
    ACT_OP = 1,
    ACT_LED = 2,
    ACT_CLOUD = 3,
    ACT_SD = 4,
    ACT_INPUT = 5,
    ACT_TAG = 6,
    ACT_LOG = 7,
    ACT_AUDIO = 8,
    ACT_WIFI = 9,
    ACT_COUNT
} activity_t;

/* Panic identifiers (implementation-defined). */
typedef enum
{
    PANIC_ASSERT = 1, /* value: line */
    PANIC_STACK_OVERFLOW,
    PANIC_EXCEPTION, /* value: exception number */
    PANIC_BUILDBLOCK,
    PANIC_PLATFORM,    /* value: bootloader word */
    PANIC_SD_HANG,     /* value: command */
    PANIC_SD_RECOVERY, /* value: sector */
    PANIC_SD_DATA,     /* data-phase error, value: sector */
    PANIC_SD_SYSTEM,   /* second failure in the system area */
    PANIC_SD_START,
    PANIC_SD_CONTENT,
    PANIC_STALL, /* value: stalled activities */
    PANIC_ISR_STACK,
    PANIC_BUSY, /* value: 900000 */
    PANIC_WAIT,
    PANIC_NWP,
    PANIC_FULL,  /* an internal queue or table is full */
    PANIC_STATE, /* an internal state that should not occur */
} panic_id_t;

/* Retained record: kind in bits 0-3 of word 1, version bit
 * 30 of word 2 marks a fault or progress record. */
#define REC_VERSION (1u << 30)
#define KIND_WATCHDOG 13u
#define KIND_PROGRESS 14u
#define KIND_HARD 15u

/* Step 1: marks the fault handling; false if a fault is
 * handled already. */
bool fault_claim(void);
/* The red LED level of a fault (0 off), where the LED can be driven. */
void fault_led(uint8_t red);

/* The activity number comes from the calling task
 * (0 in an interrupt handler); msg may be NULL (at most 79 characters). */
void panic(panic_id_t id, uint32_t value, const char *msg) __attribute__((noreturn));
/* The same for a fault of a given activity (for example the SD card,
 * whatever task made the access). */
void panic_from(activity_t act, panic_id_t id, uint32_t value, const char *msg) __attribute__((noreturn));
/* A panic, hard fault or watchdog expiry is being handled: the LED is no
 * longer changed by anything else, no progress record is stored and the
 * monitoring round does not run. */
bool fault_active(void);

/* Phase of the box for the end of a panic. */
typedef enum
{
    PHASE_NORMAL,
    PHASE_PREPARING_OFF, /* idle power-off from its start */
    PHASE_RESTARTING,    /* restart after an interrupted power-off */
} power_phase_t;
void panic_power_phase(power_phase_t p);

/* Start reason: the PRCM reset cause, PRCM_HIB_EXIT (7) for
 * a hibernation exit. */
unsigned start_reason(void);
/* Evaluates the retained record and stores the progress
 * record. Once at start, before the activities. */
void retained_start(void);
/* The fault record found at this start; false if none. */
bool retained_fault(uint32_t rec[2]);
/* The state record found at a hibernation exit, if not all zero. */
bool retained_state(uint32_t rec[2]);
/* Stores the state record (before an off state or a
 * restart); ignored while a fault is handled. */
void retained_store_state(uint32_t w1, uint32_t w2);
/* Stores the progress record with another marker. */
void retained_progress(uint32_t marker);

/* appends the fault record found at start to PM1.LOG and
 * PM2.LOG. Once after the card has been mounted. */
void fault_report_sd(void);

/* Exception entries (startup.c, watchdog.c). */
void fault_hard(const uint32_t *frame) __attribute__((noreturn));
void fault_watchdog(void) __attribute__((noreturn));
/* An MCU reset without CFW_HIBERNATE. Masks interrupts. */
void restart_hibernate(void) __attribute__((noreturn));

/* Off state, last steps of the power-off: wake by the ears (falling edge,
 * GPIO2, GPIO4) and the station (rising edge, GPIO17), hibernation; an MCU
 * reset without CFW_HIBERNATE. Masks interrupts. */
void off_state_enter(void) __attribute__((noreturn));

/* Code position of an address. */
uint16_t code_position(uint32_t addr);

#endif
