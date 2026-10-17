/*
 * Battery voltage on pin 60, ADC channel 3.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/adc.h"
#include "driverlib/pin.h"
#include "driverlib/utils.h"
#include "battery.h"
#include "tick.h"

static bool ready;
static volatile uint32_t last_mv, last_raw;

uint32_t battery_last_mv(void)
{
    return last_mv;
}

uint32_t battery_last_raw(void)
{
    return last_raw;
}

/* Bits 13-2 hold the sample; the rest is masked in case the
 * bootloader left the ADC time-stamp timer running. No word within 100 ms
 * gives 0 (0 mV, which power.c rejects as implausible) instead of a hang. */
static uint32_t word(void)
{
    uint32_t t0 = tick_ms();
    while (!MAP_ADCFIFOLvlGet(ADC_BASE, ADC_CH_3))
    {
        if (tick_ms() - t0 > 100)
        {
            return 0;
        }
    }
    return MAP_ADCFIFORead(ADC_BASE, ADC_CH_3) & 0x3FFCu;
}

static uint32_t to_mv(uint32_t w)
{
    uint32_t v = (w / 2) * 20000u / 26173u;
    return v > 89u ? v - 89u : 0;
}

uint32_t battery_measure(void)
{
    if (!ready)
    {
        MAP_PinTypeADC(PIN_60, PIN_MODE_255);
        MAP_ADCChannelEnable(ADC_BASE, ADC_CH_3);
        MAP_ADCEnable(ADC_BASE);
        (void)word();
        tick_wait(30);
        ready = true;
    }
    uint32_t a = word();
    tick_wait(73);
    uint32_t b = word();
    last_raw = b > a ? word() : a;
    last_mv = to_mv(last_raw);
    return last_mv;
}

#if CFW_HIBERNATE
/* before the scheduler, polled. */
uint32_t battery_wake_measure(void)
{
    MAP_PinTypeADC(PIN_60, PIN_MODE_255);
    MAP_ADCChannelEnable(ADC_BASE, ADC_CH_3);
    MAP_ADCEnable(ADC_BASE);
    uint32_t w[3];
    for (int i = 0; i < 4; i++)
    {
        while (!MAP_ADCFIFOLvlGet(ADC_BASE, ADC_CH_3))
            ;
        uint32_t v = MAP_ADCFIFORead(ADC_BASE, ADC_CH_3);
        if (i == 0)
        {
            UtilsDelay(30u * (80000u / 3u)); /* discard one word, 30 ms */
        }
        else
        {
            w[i - 1] = v;
        }
        if (i == 2 && w[1] <= w[0])
        {
            break;
        }
    }
    uint32_t r = w[1] > w[0] ? w[2] : w[0];
    uint32_t mv = (r / 2) * 20000u / 26173u;
    return mv > 89u ? mv - 89u : 0;
}
#endif
