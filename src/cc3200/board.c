/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_gpio.h"
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/utils.h"
#include "board.h"
#include "debug.h"
#include "led.h"
#include "nfc.h"
#include "sd.h"

/* GPIO pins: port base and bit. */
typedef struct
{
    uint32_t pin;
    uint32_t base;
    uint8_t bit;
} gpio_t;

static const gpio_t PIN_STATION = {PIN_08, GPIOA2_BASE, 0x02};    /* GPIO17 */
static const gpio_t PIN_ACCEL_INT = {PIN_15, GPIOA2_BASE, 0x40};  /* GPIO22 */
static const gpio_t PIN_NFC_CS = {PIN_16, GPIOA2_BASE, 0x80};     /* GPIO23 */
static const gpio_t PIN_NFC_IRQ = {PIN_18, GPIOA3_BASE, 0x10};    /* GPIO28 */
static const gpio_t PIN_UNUSED20 = {PIN_20, GPIOA3_BASE, 0x20};   /* GPIO29 */
static const gpio_t PIN_EAR_BIG = {PIN_57, GPIOA0_BASE, 0x04};    /* GPIO2 */
static const gpio_t PIN_SD_POWER = {PIN_58, GPIOA0_BASE, 0x08};   /* GPIO3, low = on */
static const gpio_t PIN_EAR_SMALL = {PIN_59, GPIOA0_BASE, 0x10};  /* GPIO4 */
static const gpio_t PIN_PERI_POWER = {PIN_61, GPIOA0_BASE, 0x40}; /* GPIO6, high = on */
static const gpio_t PIN_PERI_RESET = {PIN_62, GPIOA0_BASE, 0x80}; /* GPIO7, low = reset */

/* I2C lines as GPIO for the line check. */
static const gpio_t PIN_SCL = {PIN_03, GPIOA1_BASE, 0x10}; /* GPIO12 */
static const gpio_t PIN_SDA = {PIN_04, GPIOA1_BASE, 0x20}; /* GPIO13 */

static bool scl_start, sda_start;
static uint32_t loader_pins;

static void gpio_setup(const gpio_t *g, bool output, bool level)
{
    /* Push-pull, 2 mA, no pull; PinConfigSet also switches off the analog
     * isolation of the ADC-capable pads. */
    PinConfigSet(g->pin, PIN_STRENGTH_2MA, PIN_TYPE_STD);
    PinModeSet(g->pin, PIN_MODE_0);
    MAP_GPIODirModeSet(g->base, g->bit, output ? GPIO_DIR_MODE_OUT : GPIO_DIR_MODE_IN);
    if (output)
    {
        MAP_GPIOPinWrite(g->base, g->bit, level ? g->bit : 0);
    }
}

static void gpio_write(const gpio_t *g, bool level)
{
    MAP_GPIOPinWrite(g->base, g->bit, level ? g->bit : 0);
}

static bool gpio_read(const gpio_t *g)
{
    return MAP_GPIOPinRead(g->base, g->bit) != 0;
}

void board_early_init(void)
{
    /* Step 1 (UART) is debug_init(), called before. */

    /* Step 2: outputs in the order 62, 16, 61, 58, then the inputs. */
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA0, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA2, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA3, PRCM_RUN_MODE_CLK);
    /* Diagnostic: pins 61 and 62 as the boot loader left them (mux mode,
     * direction, level), for the I2C lines found low at start. */
    loader_pins = PinModeGet(PIN_61) << 12 | PinModeGet(PIN_62) << 8 |
                  (HWREG(GPIOA0_BASE + GPIO_O_GPIO_DIR) & 0xC0) | (MAP_GPIOPinRead(GPIOA0_BASE, 0xC0) >> 6);
    gpio_setup(&PIN_PERI_RESET, true, false);
    gpio_setup(&PIN_NFC_CS, true, true);
    gpio_setup(&PIN_PERI_POWER, true, true);
    gpio_setup(&PIN_SD_POWER, true, true);
    gpio_setup(&PIN_STATION, false, false);
    gpio_setup(&PIN_ACCEL_INT, false, false);
    gpio_setup(&PIN_NFC_IRQ, false, false);
    gpio_setup(&PIN_UNUSED20, false, false);
    gpio_setup(&PIN_EAR_BIG, false, false);
    gpio_setup(&PIN_EAR_SMALL, false, false);

    /* Step 3: LED PWM channels, red, green, blue, all at level 0. */
    led_init();

    /* Step 4: SD supply on. */
    gpio_write(&PIN_SD_POWER, false);

    /* Step 5: general-purpose SPI for the NFC reader. The SD host pins
     * (step 6) are set in sd_host_init(), the I2S pins (step 7) when the
     * port starts. */
    nfc_spi_init();

    /* The SD bootloader leaves the peripheral supply off (pin 61 low): it
     * comes on in step 2. Codec and accelerometer must see it settled
     * before their reset is released in step 9, and the I2C pull-ups hang
     * on it too; within the 50 ms the start-up allows. */
    if (!(loader_pins & 0x01))
    {
        UtilsDelay(20u * (80000u / 3u)); /* 20 ms */
    }

    /* Step 8: I2C line check, then the pins go to I2C mode. */
    gpio_setup(&PIN_SCL, false, false);
    gpio_setup(&PIN_SDA, false, false);
    scl_start = gpio_read(&PIN_SCL);
    sda_start = gpio_read(&PIN_SDA);
    MAP_PinTypeI2C(PIN_03, PIN_MODE_5);
    MAP_PinTypeI2C(PIN_04, PIN_MODE_5);

    /* Step 9: peripheral reset released. */
    gpio_write(&PIN_PERI_RESET, true);

    /* Step 10: SD host controller at 24 MHz. */
    sd_host_init();
}

uint32_t board_loader_pins(void)
{
    return loader_pins;
}

bool board_scl_high_at_start(void)
{
    return scl_start;
}

bool board_sda_high_at_start(void)
{
    return sda_start;
}

bool board_on_station(void)
{
    return gpio_read(&PIN_STATION);
}

bool board_ear_big_pressed(void)
{
    return !gpio_read(&PIN_EAR_BIG);
}

bool board_ear_small_pressed(void)
{
    return !gpio_read(&PIN_EAR_SMALL);
}

void board_sd_supply(bool on)
{
    gpio_write(&PIN_SD_POWER, !on);
}

void board_peripheral_supply(bool on)
{
    gpio_write(&PIN_PERI_POWER, on);
}

void board_peripheral_reset(bool asserted)
{
    gpio_write(&PIN_PERI_RESET, !asserted);
}

void board_nfc_select(bool selected)
{
    gpio_write(&PIN_NFC_CS, !selected);
}

bool board_nfc_irq(void)
{
    return gpio_read(&PIN_NFC_IRQ);
}

/* interrupt on the rising edge of pin 18. */
void board_nfc_irq_enable(void)
{
    MAP_GPIOIntTypeSet(PIN_NFC_IRQ.base, PIN_NFC_IRQ.bit, GPIO_RISING_EDGE);
    MAP_GPIOIntClear(PIN_NFC_IRQ.base, PIN_NFC_IRQ.bit);
    MAP_GPIOIntEnable(PIN_NFC_IRQ.base, PIN_NFC_IRQ.bit);
    MAP_IntPrioritySet(INT_GPIOA3, 3 << 5);
    MAP_IntEnable(INT_GPIOA3);
}

void gpioa3_handler(void)
{
    unsigned long status = MAP_GPIOIntStatus(PIN_NFC_IRQ.base, true);
    MAP_GPIOIntClear(PIN_NFC_IRQ.base, status);
    if (status & PIN_NFC_IRQ.bit)
    {
        nfc_irq_from_isr();
    }
}

bool board_accel_int(void)
{
    return gpio_read(&PIN_ACCEL_INT);
}
