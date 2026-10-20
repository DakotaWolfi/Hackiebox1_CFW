/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "port.h"
#include "panic_port.h"
#include "buildblock.h"
#include "clock.h"
#include "debug.h"
#include "led.h"
#include "monitor.h"
#include "panic.h"
#include "tick.h"
#include "version.h"
#include "watchdog.h"

static volatile bool handling;
static volatile uint8_t phase;
static uint32_t found[2]; /* record found at start */
static bool found_fault, found_state;
static unsigned reason;

/* Marks the fault handling (step 1); false if a fault is handled already. */
bool fault_claim(void)
{
    uint32_t pm = port_irq_save();
    bool first = !handling;
    handling = true;
    port_irq_restore(pm);
    return first;
}

bool fault_active(void)
{
    return handling;
}

void panic_power_phase(power_phase_t p)
{
    phase = (uint8_t)p;
}

static uint32_t slot_bits(void)
{
    return buildblock_slot() & 3u;
}

unsigned start_reason(void)
{
    return reason;
}

void retained_start(void)
{
    uint32_t w1, w2;
    port_retained_read(&w1, &w2);
    reason = port_reset_cause();
    bool take = reason == START_HIB_EXIT;
#if !CFW_HIBERNATE
    /* The own restarts are MCU resets here: a panic, hard fault or watchdog
     * record they left is reported as after a hibernate restart. */
    if ((reason == START_CORE_RESET || reason == START_MCU_RESET || reason == START_WDT_RESET) &&
        (w2 & REC_VERSION) && (w1 & 15) != KIND_PROGRESS)
    {
        take = true;
    }
    /* The same for the state record of the off state without hibernation
     * (bit 31 of word 2, pwrstate.h). */
    if (reason == START_MCU_RESET && !(w2 & REC_VERSION) && (w2 & (1u << 31)))
    {
        take = true;
    }
#endif
    if (take && (w2 & REC_VERSION))
    {
        found_fault = true;
    }
    else if (take && (w1 || w2))
    {
        found_state = true;
    }
    found[0] = w1;
    found[1] = w2;
    /* Start banner: reset cause, the raw record and what it is taken for
     * (F fault record, version in bits 30-31 of word 2; S state record,
     * still checked by pwrstate_decode; - ignored). */
    debug_printf("start: cause %u rec %08lx %08lx %c\n", reason, (unsigned long)w1, (unsigned long)w2,
                 found_fault ? 'F' : found_state ? 'S'
                                                 : '-');
    retained_progress(1);
}

bool retained_fault(uint32_t rec[2])
{
    if (found_fault)
    {
        memcpy(rec, found, sizeof found);
    }
    return found_fault;
}

bool retained_state(uint32_t rec[2])
{
    if (found_state)
    {
        memcpy(rec, found, sizeof found);
    }
    return found_state;
}

void retained_store_state(uint32_t w1, uint32_t w2)
{
    if (!handling)
    {
        port_retained_write(w1, w2);
    }
}

void retained_progress(uint32_t marker)
{
    if (!handling)
    {
        port_retained_write(KIND_PROGRESS | (marker & 0xFFFu) << 20,
                            (marker >> 12 & 0xFFFFFu) | slot_bits() << 20 | REC_VERSION);
    }
}

/* one 16-byte entry at the next multiple of 16 bytes. */
static void append(const char *name, const uint8_t entry[16])
{
    static FIL f;
    UINT n = 0;
    if (f_open(&f, name, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK)
    {
        debug_printf("%s not opened, fault lost\n", name);
        return;
    }
    FSIZE_t at = (f_size(&f) + 15) & ~(FSIZE_t)15;
    if (f_lseek(&f, at) != FR_OK || f_write(&f, entry, 16, &n) != FR_OK || n != 16)
    {
        debug_printf("%s not written, fault lost\n", name);
    }
    f_close(&f);
}

void fault_report_sd(void)
{
    if (!found_fault || (found[0] & 15) == ACT_SD)
    {
        return;
    }
    uint8_t e[16] = {0};
    uint32_t t = clock_now(); /* seconds since start until the clock is set */
    memcpy(e, &t, 4);
    memcpy(e + 4, &found[0], 4);
    memcpy(e + 8, &found[1], 4);
    append("PM1.LOG", e);
    append("PM2.LOG", e);
}

/* Activity number of the running task. */
static unsigned activity(void)
{
    static const struct
    {
        char name[8];
        uint8_t act;
    } map[] = {
        {"op", ACT_OP},
        {"led", ACT_LED},
        {"cloud", ACT_CLOUD},
        {"player", ACT_SD},
        {"input", ACT_INPUT},
        {"tag", ACT_TAG},
        {"rtnl", ACT_LOG},
        {"sound", ACT_AUDIO},
        {"net", ACT_WIFI},
        {"slspawn", ACT_WIFI},
    };
    if (port_in_isr())
    {
        return ACT_NONE;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return ACT_OP;
    }
    const char *n = pcTaskGetName(NULL);
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
    {
        if (!strcmp(n, map[i].name))
        {
            return map[i].act;
        }
    }
    return ACT_NONE;
}

static void delay_ms(bool paced, uint32_t ms)
{
    if (paced)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static void __attribute__((noreturn)) do_panic(unsigned act, uint32_t ret, panic_id_t id, uint32_t value,
                                               const char *msg)
{
    bool paced = debug_task_context();
    if (!fault_claim())
    {
        /* Step 1: a fault is handled already; stop here for good. */
        if (paced)
        {
            vTaskSuspend(NULL);
        }
        for (;;)
            ;
    }
    uint16_t pos = code_position(ret);
    watchdog_feed();                                                      /* step 3 */
    port_retained_write(act | (uint32_t)id << 4 | (value & 0xFFFu) << 20, /* step 4 */
                        (value >> 12 & 0xFFFFFu) | slot_bits() << 20 | REC_VERSION);
    debug_printf("PANIC %s act %u id %u pos %04x val 0x%08lx %.79s\n", CFW_VERSION_STRING, act,
                 (unsigned)id, pos, (unsigned long)value, msg ? msg : ""); /* step 5 */
    debug_flush();
    /* Step 6 (log drain) comes with the log upload. Step 7. */
    if (phase == PHASE_RESTARTING)
    {
        restart_hibernate();
    }
    if (phase != PHASE_PREPARING_OFF)
    {
        for (int i = 0; i < 20; i++)
        {
            fault_led(144); /* the red of the LED patterns */
            delay_ms(paced, 500);
            fault_led(0);
            delay_ms(paced, 500);
        }
    }
    off_state_enter();
}

void panic(panic_id_t id, uint32_t value, const char *msg)
{
    do_panic(activity(), (uint32_t)__builtin_return_address(0), id, value, msg);
}

void panic_from(activity_t act, panic_id_t id, uint32_t value, const char *msg)
{
    do_panic(port_in_isr() ? ACT_NONE : act, (uint32_t)__builtin_return_address(0), id, value, msg);
}

void fault_watchdog(void)
{
    if (!fault_claim())
    {
        restart_hibernate();
    }
    port_retained_write(KIND_WATCHDOG | (uint32_t)monitor_op_position() << 4 | slot_bits() << 24, REC_VERSION);
    fault_led(0);
    debug_write("watchdog expiry\n", 64);
    restart_hibernate();
}
