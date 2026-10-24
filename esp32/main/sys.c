/*
 * System part of the fault handling and power management on the ESP32 box: restart, the
 * retained record in RTC memory, the reset cause, the off state and the
 * hibernate restart as deep sleep, the code position of an address and
 * the LED of a fault.
 *
 * The build sets CFW_HIBERNATE 0: the own restarts and a watchdog end in a
 * reset whose record the next start takes (panic.c); the off state is deep
 * sleep, whose wake-up counts as the hibernation exit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "debug.h"
#include "esp32.h"
#include "led.h"
#include "panic.h"
#include "panic_port.h"
#include "port.h"

#define RETAINED_MAGIC 0x52544E31u /* "RTN1" */

portMUX_TYPE port_mux = portMUX_INITIALIZER_UNLOCKED;

/* two words kept by a reset and by deep sleep; the check word
 * tells them from the random content after power-on. */
static RTC_NOINIT_ATTR uint32_t retained[3];

void port_retained_write(uint32_t w1, uint32_t w2)
{
    retained[0] = w1;
    retained[1] = w2;
    retained[2] = w1 ^ w2 ^ RETAINED_MAGIC;
}

void port_retained_read(uint32_t *w1, uint32_t *w2)
{
    bool valid = retained[2] == (retained[0] ^ retained[1] ^ RETAINED_MAGIC);
    *w1 = valid ? retained[0] : 0;
    *w2 = valid ? retained[1] : 0;
    /* A crash of the system (exception, stack overflow, abort) leaves no
     * record of its own: it is reported as a hard fault without position,
     * unless a panic or watchdog record is there. */
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC && (!(*w2 & REC_VERSION) || (*w1 & 15) == KIND_PROGRESS))
    {
        *w1 = KIND_HARD | 0xFFFFu << 4;
        *w2 = REC_VERSION | 0xFFFFu;
    }
}

unsigned port_reset_cause(void)
{
    switch (esp_reset_reason())
    {
    case ESP_RST_DEEPSLEEP:
        return START_HIB_EXIT;
    case ESP_RST_SW:
        return START_MCU_RESET;
    case ESP_RST_PANIC:
        return START_CORE_RESET;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return START_WDT_RESET;
    default:
        return START_POWER_ON;
    }
}

void port_restart(void)
{
    board_release_for_reset();
    esp_restart();
}

/* the offset in the image's code in units of 4 bytes, for
 * addresses in the instruction memory mapped from flash or in IRAM. */
uint16_t code_position(uint32_t addr)
{
    if (addr >= 0x42000000u && addr < 0x42000000u + 0x3FFF8u * 4u)
    {
        return (uint16_t)((addr - 0x42000000u) >> 2);
    }
    return 0xFFFE;
}

void fault_led(uint8_t r)
{
    if (led_ready())
    {
        led_set(r, 0, 0);
    }
}

/* Deep sleep with the off state's wake-up sources on this board: the
 * wake line (an ear, or the charger plugged in) low. On the charger the
 * line is already low; then the ears alone wake the box. */
static void __attribute__((noreturn)) deep_sleep(uint64_t timer_us)
{
    bool on_charger = board_wake_line() && !board_ear_big_pressed() && !board_ear_small_pressed();
    board_release_for_reset();
    board_hold_off();
    if (timer_us)
    {
        esp_sleep_enable_timer_wakeup(timer_us);
    }
    else if (on_charger)
    {
        esp_sleep_enable_ext1_wakeup_io((1ull << GPIO_NUM_7) | (1ull << GPIO_NUM_8), ESP_EXT1_WAKEUP_ANY_LOW);
        rtc_gpio_pullup_en(GPIO_NUM_7);
        rtc_gpio_pullup_en(GPIO_NUM_8);
    }
    else
    {
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_7, 0);
        rtc_gpio_pullup_en(GPIO_NUM_7);
    }
    esp_deep_sleep_start();
}

void off_state_enter(void)
{
    debug_write("off\n", 8);
    debug_flush();
    /* An ear still held would wake the box at once: wait for the release. */
    for (int i = 0; i < 500 && (board_ear_big_pressed() || board_ear_small_pressed()); i++)
    {
        esp_rom_delay_us(10000);
    }
    deep_sleep(0);
}

/* a restart through deep sleep, so that the start counts as
 * the hibernation exit. */
void restart_hibernate(void)
{
    debug_write("hibernate restart\n", 64);
    debug_flush();
    deep_sleep(10000);
}
