/*
 * Pins and supplies of the ESP32-S3 CC3200 replacement board:
 *
 *   GPIO15  supply of accelerometer, codec and LED, high = on
 *   GPIO21  supply of card and NFC reader, low = on
 *   GPIO16  codec reset, high = running
 *   GPIO7   big ear, GPIO8 small ear, active low
 *   GPIO13  wake line (NFC IRQ), low = wake source in deep sleep
 *   GPIO17/18/40 LED R/G/B
 *   GPIO1   NFC chip select (low), GPIO13 NFC interrupt request (high)
 *   GPIO5/6 I2C SDA/SCL
 *
 * This transplant board has no charger station signal wired to the ESP32,
 * so board_on_station() stays false.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "board.h"
#include "debug.h"
#include "esp32.h"
#include "led.h"
#include "nfc.h"

#define PIN_PERI_POWER GPIO_NUM_15
#define PIN_STORAGE_POWER GPIO_NUM_21
#define PIN_DAC_RESET GPIO_NUM_16
#define PIN_EAR_BIG GPIO_NUM_7
#define PIN_EAR_SMALL GPIO_NUM_8
#define PIN_WAKE GPIO_NUM_13
#define PIN_ACCEL_INT GPIO_NUM_14
#define PIN_NFC_CS GPIO_NUM_1
#define PIN_NFC_IRQ GPIO_NUM_13
#define PIN_SCL GPIO_NUM_6
#define PIN_SDA GPIO_NUM_5

static bool scl_start, sda_start;
static volatile bool station;
static adc_oneshot_unit_handle_t adc;
static StaticSemaphore_t adc_lock_mem;
static SemaphoreHandle_t adc_lock;

static void out(gpio_num_t pin, bool level)
{
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void in(gpio_num_t pin, gpio_pull_mode_t pull)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, pull);
}

/* One conversion of ADC1 channel ch (GPIO8 = channel 7, GPIO9 = channel 8)
 * in mV at the pin, with the chip's calibration; -1 on failure. */
int board_adc_mv(int ch)
{
    if (!adc)
    {
        return -1;
    }
    int raw = 0;
    xSemaphoreTake(adc_lock, portMAX_DELAY);
    esp_err_t e = adc_oneshot_read(adc, (adc_channel_t)ch, &raw);
    xSemaphoreGive(adc_lock);
    if (e != ESP_OK)
    {
        return -1;
    }
    return esp32_adc_raw_to_mv(raw);
}

void board_early_init(void)
{
    /* the off levels held through the deep sleep (board_hold_off) */
    gpio_hold_dis(PIN_PERI_POWER);
    gpio_hold_dis(PIN_STORAGE_POWER);
    gpio_hold_dis(PIN_DAC_RESET);
    /* Step 2 of the start-up on this board: codec in reset, both supplies on,
     * NFC not selected. */
    out(PIN_DAC_RESET, false);
    out(PIN_NFC_CS, true);
    out(PIN_PERI_POWER, true);
    out(PIN_STORAGE_POWER, false);
    in(PIN_EAR_BIG, GPIO_PULLUP_ONLY);
    in(PIN_EAR_SMALL, GPIO_PULLUP_ONLY);
    in(PIN_WAKE, GPIO_PULLUP_ONLY);
    in(PIN_ACCEL_INT, GPIO_FLOATING);
    in(PIN_NFC_IRQ, GPIO_FLOATING);

    /* Step 3: LED channels at level 0. */
    led_init();

    /* The battery input on ADC1, 12 dB attenuation. */
    adc_lock = xSemaphoreCreateMutexStatic(&adc_lock_mem);
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &adc) == ESP_OK)
    {
        adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        adc_oneshot_config_channel(adc, (adc_channel_t)BOARD_ADC_BATTERY, &ccfg);
        esp32_adc_calibration_init();
    }
    else
    {
        adc = NULL;
        debug_printf("board: ADC not available\n");
    }
    station = false;

    /* Step 5: SPI for the NFC reader. */
    nfc_spi_init();

    /* The supplies settle within the 50 ms the start-up allows. */
    esp_rom_delay_us(20000);

    /* Step 8: I2C line check. */
    in(PIN_SCL, GPIO_FLOATING);
    in(PIN_SDA, GPIO_FLOATING);
    scl_start = gpio_get_level(PIN_SCL);
    sda_start = gpio_get_level(PIN_SDA);

    /* Step 9: codec reset released. */
    gpio_set_level(PIN_DAC_RESET, true);
}

uint32_t board_loader_pins(void)
{
    return 0;
}

bool board_scl_high_at_start(void)
{
    return scl_start;
}

bool board_sda_high_at_start(void)
{
    return sda_start;
}

/* These four are called from the tick hook (input.c), also while the flash
 * cache is off: IRAM, and the register read instead of gpio_get_level(). */
bool IRAM_ATTR board_on_station(void)
{
    return station;
}

bool IRAM_ATTR board_ear_big_pressed(void)
{
    return !gpio_ll_get_level(&GPIO, PIN_EAR_BIG);
}

bool IRAM_ATTR board_ear_small_pressed(void)
{
    return !gpio_ll_get_level(&GPIO, PIN_EAR_SMALL);
}

bool board_wake_line(void)
{
    return !gpio_get_level(PIN_WAKE);
}

/* The card shares its supply with the NFC reader on this board: the reader
 * is initialised again after the supply was off. */
void board_sd_supply(bool on)
{
    gpio_set_level(PIN_STORAGE_POWER, !on);
    if (!on)
    {
        nfc_reinit_later();
    }
}

void board_peripheral_supply(bool on)
{
    gpio_set_level(PIN_PERI_POWER, on);
}

/* Only the codec has a reset line here. */
void board_peripheral_reset(bool asserted)
{
    gpio_set_level(PIN_DAC_RESET, !asserted);
}

void board_nfc_select(bool selected)
{
    gpio_set_level(PIN_NFC_CS, !selected);
}

bool board_nfc_irq(void)
{
    return gpio_get_level(PIN_NFC_IRQ);
}

static void nfc_isr(void *arg)
{
    (void)arg;
    nfc_irq_from_isr();
}

void board_nfc_irq_enable(void)
{
    gpio_install_isr_service(0);
    gpio_set_intr_type(PIN_NFC_IRQ, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(PIN_NFC_IRQ, nfc_isr, NULL);
    gpio_intr_enable(PIN_NFC_IRQ);
}

bool IRAM_ATTR board_accel_int(void)
{
    return gpio_ll_get_level(&GPIO, PIN_ACCEL_INT);
}

/* Before any reset: rails off. */
void board_release_for_reset(void)
{
    led_set(0, 0, 0);
    gpio_set_level(PIN_DAC_RESET, false);
    gpio_set_level(PIN_PERI_POWER, false);
    gpio_set_level(PIN_STORAGE_POWER, true);
}

/* In deep sleep the pads lose their output levels; holds keep supplies off
 * and codec in reset until wake-up. */
void board_hold_off(void)
{
    gpio_hold_en(PIN_DAC_RESET);
    gpio_hold_en(PIN_PERI_POWER);
    gpio_hold_en(PIN_STORAGE_POWER);
    gpio_deep_sleep_hold_en();
}
