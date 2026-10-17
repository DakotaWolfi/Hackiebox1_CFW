/*
 * The CC3200 part of the error handling: off state, hibernate restart and the
 * hard fault record (Cortex-M4 fault status registers), the code position
 * of an address in the image, the LED of a fault. The panic itself and
 * the retained record are shared (panic.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "driverlib/utils.h"
#include "board.h"
#include "debug.h"
#include "led.h"
#include "panic.h"
#include "panic_port.h"
#include "watchdog.h"

extern uint32_t __image_end;

uint16_t code_position(uint32_t addr)
{
    if (addr < 0x20004000u || addr >= (uint32_t)&__image_end)
    {
        return 0xFFFE;
    }
    return (uint16_t)((addr - 0x20004000u) >> 2); /* < 0xD000 */
}

/* The LED timers may not be clocked yet in an early fault. */
void fault_led(uint8_t r)
{
    if (MAP_PRCMPeripheralStatusGet(PRCM_TIMERA0) && MAP_PRCMPeripheralStatusGet(PRCM_TIMERA1))
    {
        led_set(r, 0, 0);
    }
}

#if !CFW_HIBERNATE
static unsigned wake_inputs(void)
{
    return board_ear_big_pressed() | board_ear_small_pressed() << 1 | board_on_station() << 2;
}
#endif

/* Off state (power-off wake-up sources, no box state stored). Without
 * CFW_HIBERNATE it is a polled wait with interrupts masked, so that no
 * activity runs any more, until an ear is pressed or the box is docked
 * (the edges of the hibernation wake-up), then an MCU reset; the watchdog
 * is fed in the wait (nothing else can). A panic thus ends off instead of
 * restarting at once. */
void off_state_enter(void)
{
    __asm volatile("cpsid i");
    debug_write("off\n", 8);
    debug_flush();
#if CFW_HIBERNATE
    MAP_PRCMHibernateWakeupSourceDisable(PRCM_HIB_SLOW_CLK_CTR);
    MAP_PRCMHibernateWakeUpGPIOSelect(PRCM_HIB_GPIO2 | PRCM_HIB_GPIO4, PRCM_HIB_FALL_EDGE);
    MAP_PRCMHibernateWakeUpGPIOSelect(PRCM_HIB_GPIO17, PRCM_HIB_RISE_EDGE);
    MAP_PRCMHibernateWakeupSourceEnable(PRCM_HIB_GPIO2 | PRCM_HIB_GPIO4 | PRCM_HIB_GPIO17);
    UtilsDelay(80000u); /* 3 ms */
    MAP_PRCMHibernateEnter();
#else
    fault_led(0);
    for (unsigned prev = wake_inputs(), now;; prev = now)
    {
        watchdog_feed();
        UtilsDelay(20u * (80000u / 3u)); /* 20 ms */
        now = wake_inputs();
        if (now & ~prev)
        {
            break;
        }
    }
    debug_write("wake\n", 8);
    debug_flush();
#endif
    MAP_PRCMMCUReset(true);
    for (;;)
        ;
}

void restart_hibernate(void)
{
    __asm volatile("cpsid i");
    debug_write("hibernate restart\n", 64);
    debug_flush();
#if CFW_HIBERNATE
    /* Network processor off (CC3200 SDK cc_pal.c). */
    HWREG(0x400F70B8) = 1; /* interrupt to the network processor */
    UtilsDelay(160000u);   /* 6 ms */
    HWREG(0x400F70B0) = 1; /* cleared */
    UtilsDelay(160000u);
    HWREG(0x4402E16C) |= 2; /* shut-down request */
    MAP_PRCMHibernateWakeUpGPIOSelect(PRCM_HIB_GPIO17, PRCM_HIB_RISE_EDGE);
    MAP_PRCMHibernateIntervalSet(330);
    MAP_PRCMHibernateWakeupSourceEnable(PRCM_HIB_SLOW_CLK_CTR | PRCM_HIB_GPIO17);
    MAP_PRCMHibernateEnter();
#endif
    MAP_PRCMMCUReset(true);
    for (;;)
        ;
}

void fault_hard(const uint32_t *frame)
{
    if (!fault_claim())
    {
        restart_hibernate();
    }
    uint32_t hfsr = HWREG(0xE000ED2C), cfsr = HWREG(0xE000ED28), bfar = HWREG(0xE000ED38);
    uint32_t w1 = KIND_HARD, w2 = REC_VERSION;
    if (hfsr & (1u << 1))
    {
        w1 |= 1u << 20; /* vector table read */
    }
    if (hfsr & (1u << 30))
    { /* escalated */
        w1 |= (uint32_t)code_position(frame[6]) << 4;
        w2 |= code_position(frame[5]);
        if (cfsr & (1u << 15))
        {
            w1 |= 1u << 21;
            w2 |= (bfar >> 18 & 0x3FFFu) << 16;
        }
        if (cfsr & (3u << 11))
        {
            w1 |= 1u << 22; /* stacking, unstacking */
        }
        if (cfsr & (1u << 10))
        {
            w1 |= 1u << 23;
        }
        if (cfsr & (1u << 9))
        {
            w1 |= 1u << 24;
        }
        if (cfsr & (1u << 8))
        {
            w1 |= 1u << 25;
        }
        if (cfsr & (1u << 25))
        {
            w1 |= 1u << 26; /* divide by zero */
        }
        if (cfsr & (1u << 24))
        {
            w1 |= 1u << 27; /* unaligned */
        }
        if (cfsr & (0xFu << 16 | 1u << 13 | 1u << 5))
        {
            w1 |= 1u << 28; /* other usage fault, lazy FP */
        }
    }
    else
    {
        w1 |= 0xFFFFu << 4;
        w2 |= 0xFFFFu;
    }
    port_retained_write(w1, w2);
    fault_led(0);
    restart_hibernate();
}
