/*
 * LED PWM: red GPIO19, green GPIO18,
 * blue GPIO17, active high, on the LEDC peripheral at 8 bits.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "driver/ledc.h"
#include "led.h"

static const int pins[3] = {19, 18, 17};
static bool ready;

void led_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK)
    {
        return;
    }
    for (int i = 0; i < 3; i++)
    {
        ledc_channel_config_t c = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        ledc_channel_config(&c);
    }
    ready = true;
}

bool led_ready(void)
{
    return ready;
}

void led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!ready)
    {
        return;
    }
    const uint8_t v[3] = {red, green, blue};
    for (int i = 0; i < 3; i++)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, v[i] > 254 ? 254 : v[i]);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
}
