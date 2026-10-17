/*
 * LED PWM channels.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

void led_init(void);
/* PWM levels 0..254 per channel; larger values are taken as 254. */
void led_set(uint8_t red, uint8_t green, uint8_t blue);

#endif
