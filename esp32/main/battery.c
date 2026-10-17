/*
 * Battery voltage on GPIO9 (ADC1 channel 8) behind a 100 kOhm / 33 kOhm
 * divider (pinout of the reverse-engineering wiki). The ADC is
 * set up in board.c; the conversion to mV uses the chip's calibration.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"
#include "FreeRTOS.h"
#include "task.h"
#include "battery.h"
#include "esp32.h"
#include "tick.h"

static adc_cali_handle_t cali;
static volatile uint32_t last_mv, last_raw;

void esp32_adc_calibration_init(void)
{
    adc_cali_curve_fitting_config_t c = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&c, &cali) != ESP_OK)
    {
        cali = NULL;
    }
}

/* Without calibration data: full scale about 3100 mV at 12 dB. */
int esp32_adc_raw_to_mv(int raw)
{
    int mv;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK)
    {
        return mv;
    }
    return raw * 3100 / 4095;
}

uint32_t battery_last_mv(void)
{
    return last_mv;
}

uint32_t battery_last_raw(void)
{
    return last_raw;
}

/* 16 samples 5 ms apart, the mean of the middle 8: one sample taken in a
 * dip of the supply (speaker peak, Wi-Fi transmission) or a noisy one
 * moves the result by little; the divider multiplies an error at the pin
 * by four. Before the scheduler runs (wake-up check) the waits are busy. */
#define SAMPLES 16

static uint32_t measure(void)
{
    int v[SAMPLES];
    bool sched = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    for (int i = 0; i < SAMPLES; i++)
    {
        if (i)
        {
            if (sched)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            else
            {
                esp_rom_delay_us(5000);
            }
        }
        v[i] = board_adc_mv(BOARD_ADC_BATTERY);
        if (v[i] < 0)
        {
            return 0; /* 0 mV: implausible, ignored by power.c */
        }
    }
    for (int i = 1; i < SAMPLES; i++)
    {
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--)
        {
            int t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    }
    uint32_t sum = 0;
    for (int i = SAMPLES / 4; i < SAMPLES - SAMPLES / 4; i++)
    {
        sum += (uint32_t)v[i];
    }
    /* the pin voltage in mV (start-up log, battery report) */
    last_raw = sum / (SAMPLES / 2);
    /* divider (100 + 33) / 33 */
    return last_raw * 133u / 33u;
}

uint32_t battery_measure(void)
{
    last_mv = measure();
    return last_mv;
}

uint32_t battery_wake_measure(void)
{
    return measure();
}
