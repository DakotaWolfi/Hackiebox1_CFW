/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/timer.h"
#include "led.h"

#define PWM_LOAD 39999u
#define PWM_STEP 157u

static void channel_setup(uint32_t base, uint32_t timer)
{
    MAP_TimerPrescaleSet(base, timer, 0);
    MAP_TimerControlLevel(base, timer, true); /* inverted output */
    MAP_TimerLoadSet(base, timer, PWM_LOAD);
    MAP_TimerMatchSet(base, timer, PWM_LOAD);
}

static void channel_level(uint32_t base, uint32_t timer, uint8_t level)
{
    if (level > 254)
    {
        level = 254;
    }
    MAP_TimerEnable(base, timer);
    /* With the inverted output the pin is high for `match` cycles of the
     * period, as in TI's PWM example. The value PWM_LOAD - level x 157 that
     * a direct mapping implies inverts the brightness: on the box (test of
     * 2026-10-12) full colours and blinks stayed dark, only the blue pulse
     * of setup mode was visible. */
    MAP_TimerMatchSet(base, timer, (uint32_t)level * PWM_STEP);
}

void led_init(void)
{
    MAP_PRCMPeripheralClkEnable(PRCM_TIMERA0, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_TIMERA1, PRCM_RUN_MODE_CLK);

    /* Order red, green, blue (start-up step 3). */
    MAP_PinTypeTimer(PIN_19, PIN_MODE_8);
    MAP_TimerConfigure(TIMERA1_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM | TIMER_CFG_B_PWM);
    channel_setup(TIMERA1_BASE, TIMER_B);
    channel_level(TIMERA1_BASE, TIMER_B, 0);

    MAP_PinTypeTimer(PIN_21, PIN_MODE_9);
    channel_setup(TIMERA1_BASE, TIMER_A);
    channel_level(TIMERA1_BASE, TIMER_A, 0);

    MAP_PinTypeTimer(PIN_17, PIN_MODE_5);
    MAP_TimerConfigure(TIMERA0_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM);
    channel_setup(TIMERA0_BASE, TIMER_A);
    channel_level(TIMERA0_BASE, TIMER_A, 0);
}

void led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    channel_level(TIMERA1_BASE, TIMER_B, red);
    channel_level(TIMERA1_BASE, TIMER_A, green);
    channel_level(TIMERA0_BASE, TIMER_A, blue);
}
