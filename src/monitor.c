/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "port.h"
#include "debug.h"
#include "monitor.h"
#include "panic.h"
#include "tick.h"
#include "watchdog.h"

#define LIMIT_MS 20000u
#define LIMIT_UPDATE_MS 40000u
#define STALL_PANIC_MS 600000u
#define BUSY_PANIC_MS 900000u
#define MONITORED (1u << ACT_CLOUD | 1u << ACT_SD | 1u << ACT_INPUT | 1u << ACT_TAG | 1u << ACT_LOG)
#define STACK_LOW_WORDS 48u /* report threshold */
#define ISR_STACK_MIN 16u   /* bytes never used */
#define PAINT 0xA5A5A5A5u

static volatile uint32_t mark[ACT_COUNT];
static volatile uint32_t active; /* activities that have marked */
static volatile bool update_round;
static volatile uint8_t busy; /* bit per source */
static volatile uint32_t busy_at, sd_at;
static uint32_t prev_stalled, stall_first, stall_since;
static bool stall_noted;
static uint16_t op_pos;

void monitor_mark(activity_t a)
{
    mark[a] = tick_ms();
    active |= 1u << a;
}

void monitor_update_round(bool on)
{
    update_round = on;
}

bool monitor_in_update_round(void)
{
    return update_round;
}

/* Each source sets or clears its own bit; any call counts as setting the
 * box activity. */
static void busy_set(uint8_t bit, bool on)
{
    CRITICAL_ENTER();
    busy = on ? busy | bit : busy & ~bit;
    busy_at = tick_ms();
    CRITICAL_EXIT();
}

void monitor_box_activity(bool b)
{
    busy_set(1, b);
}

void monitor_sync_activity(bool b)
{
    busy_set(2, b);
}

void monitor_power_activity(bool b)
{
    busy_set(4, b);
}

void monitor_sd_transfer(void)
{
    sd_at = tick_ms();
}

uint16_t monitor_op_position(void)
{
    return op_pos;
}

void monitor_feed(void)
{
    if (!fault_active())
    {
        watchdog_feed();
    }
}

static uint32_t stalled_now(uint32_t now)
{
    uint32_t set = 0, act = active & MONITORED;
    for (unsigned a = 0; a < ACT_COUNT; a++)
    {
        if (!(act & 1u << a))
        {
            continue;
        }
        uint32_t limit = a == ACT_CLOUD && update_round ? LIMIT_UPDATE_MS : LIMIT_MS;
        /* A mark set by another task after now was read is newer than
         * now: as unsigned difference it looked 49 days old (device run
         * 2026-10-14, "mark 4294967295 ms ago"). */
        int32_t age = (int32_t)(now - mark[a]);
        if (age > (int32_t)limit)
        {
            set |= 1u << a;
        }
    }
    return set;
}

static void stalls(uint32_t now)
{
    uint32_t s = stalled_now(now);
    uint32_t newly = s & ~prev_stalled, ended = prev_stalled & ~s;
    for (unsigned a = 0; a < ACT_COUNT; a++)
    {
        if (newly & 1u << a)
        {
            debug_printf("mon: act %u stalled %lu ms\n", a,
                         (unsigned long)(now - mark[a]));
        }
        if (ended & 1u << a)
        {
            debug_verbose("mon: act %u ok again\n", a);
        }
    }
    if (newly)
    {
        debug_printf("mon: stalled %03lx\n", (unsigned long)s);
    }
    if (s && !prev_stalled && !stall_first)
    {
        stall_first = now;
    }
    if (!s && prev_stalled)
    {
        debug_printf("mon: stalls over, %lu ms\n", (unsigned long)(now - stall_first));
        stall_first = 0;
    }
    prev_stalled = s;

    if (!s)
    {
        stall_noted = false;
    }
    else if (!stall_noted)
    {
        stall_noted = true;
        stall_since = now;
    }
    else if (now - stall_since >= STALL_PANIC_MS)
    {
        panic(PANIC_STALL, s, "activities stalled");
    }
}

/* the later of the box activity change and the last transfer. */
static void busy_check(uint32_t now)
{
    if (!busy)
    {
        return;
    }
    uint32_t a = now - busy_at, b = now - sd_at;
    if ((a < b ? a : b) >= BUSY_PANIC_MS)
    {
        debug_printf("mon: busy %x\n", busy); /* 1 player, 2 sync, 4 power */
        panic(PANIC_BUSY, BUSY_PANIC_MS, "busy state without progress");
    }
}

/* Stack reserve, interrupt handling: the main stack below the deepest use so
 * far is painted once; fewer than 16 bytes never used is a panic. On the
 * ESP32 the system's own interrupt stack check does this. */
#ifdef ESP_PLATFORM
static void isr_stack(void)
{
}
#else
extern uint32_t __stack_top, __stack_size;

static void isr_stack(void)
{
    static bool painted;
    uint32_t *bottom = (uint32_t *)((uint32_t)&__stack_top - (uint32_t)&__stack_size);
    if (!painted)
    {
        painted = true;
        uint32_t msp, pm;
        __asm volatile("mrs %0, primask\n cpsid i" : "=r"(pm)::"memory");
        __asm volatile("mrs %0, msp" : "=r"(msp));
        for (uint32_t *p = bottom; p < (uint32_t *)(msp - 64); p++)
        {
            *p = PAINT;
        }
        __asm volatile("msr primask, %0" ::"r"(pm) : "memory");
    }
    unsigned unused = 0;
    while (unused < ISR_STACK_MIN / 4 && bottom[unused] == PAINT)
    {
        unused++;
    }
    if (unused < ISR_STACK_MIN / 4)
    {
        panic(PANIC_ISR_STACK, unused * 4, "interrupt stack exhausted");
    }
}
#endif

/* Stack reserve, activities: a report below the threshold and at every new

 * low. */
static void task_stacks(void)
{
    static const char *const names[] = {"op", "input", "player", "tag", "sound", "led",
                                        "net", "cloud", "rtnl", "slspawn", "IDLE"};
    static uint16_t low[sizeof names / sizeof names[0]];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
    {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (!t)
        {
            continue;
        }
        uint16_t w = (uint16_t)TASK_STACK_FREE_WORDS(t);
        if (w < STACK_LOW_WORDS && (!low[i] || w < low[i]))
        {
            debug_printf("mon: stack %s %u words\n", names[i], w);
        }
        if (!low[i] || w < low[i])
        {
            low[i] = w ? w : 1;
        }
    }
}

void monitor_round(void)
{
    if (fault_active())
    {
        return;
    }
    op_pos = code_position((uint32_t)__builtin_return_address(0));
    uint32_t now = tick_ms();
    stalls(now);
    busy_check(now);
    isr_stack();
    task_stacks();
    if (!fault_active())
    {
        watchdog_feed();
    }
}
