/*
 * SD card on the SDMMC host of the ESP32-S3, 1-bit bus: CLK GPIO41, CMD
 * GPIO42, DAT0 GPIO47; supply GPIO21, shared with the NFC reader. The card rules of the
 * CC3200 box keep their meaning: identification with bus recoveries for up to 20 s
 * at start, a failed transfer is followed by a bus recovery and
 * one repetition, a second failure in the system area is a panic, in the
 * data area the access fails. The commands of the
 * transfers are the driver's (ESP-IDF, SD/SDIO/MMC driver).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "debug.h"
#include "journal.h"
#include "monitor.h"
#include "nfc.h"
#include "panic.h"
#include "sd.h"
#include "tick.h"

static sdmmc_host_t host = SDMMC_HOST_DEFAULT();
static sdmmc_card_t card;
static bool host_up, ready;
static uint32_t system_end;
static uint32_t clock_khz = SDMMC_FREQ_DEFAULT;

void sd_host_init(void)
{
    if (host_up)
    {
        return;
    }
    if (sdmmc_host_init() != ESP_OK)
    {
        debug_printf("sd: host FAILED\n");
        return;
    }
    sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    s.width = 1;
    s.clk = GPIO_NUM_41;
    s.cmd = GPIO_NUM_42;
    s.d0 = GPIO_NUM_47;
    s.d1 = GPIO_NUM_NC;
    s.d2 = GPIO_NUM_NC;
    s.d3 = GPIO_NUM_NC;
    s.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &s) != ESP_OK)
    {
        debug_printf("sd: slot FAILED\n");
        return;
    }
    host.slot = SDMMC_HOST_SLOT_1;
    host_up = true;
}

void sd_host_setup(bool pull_up, uint32_t card_clock)
{
    (void)pull_up;
    clock_khz = card_clock / 1000u;
}

uint32_t sd_host_clock(void)
{
    return clock_khz * 1000u;
}

bool sd_identify(void)
{
    ready = false;
    sd_host_init();
    if (!host_up)
    {
        return false;
    }
    host.max_freq_khz = (int)clock_khz;
    memset(&card, 0, sizeof card);
    esp_err_t e = sdmmc_card_init(&host, &card);
    if (e != ESP_OK)
    {
        debug_printf("sd: identification %d\n", e);
        return false;
    }
    ready = true;
    debug_printf("sd: card %s, %lu sectors, %d kHz, %d-bit\n", card.cid.name, (unsigned long)card.csd.capacity,
                 card.real_freq_khz, card.log_bus_width ? 1 << card.log_bus_width : 1);
    return true;
}

/* Card recovery on this board: supply off 100 ms (the NFC reader with it), on,
 * 200 ms, 1000 ms, identification at the default clock. */
bool sd_recover(void)
{
    ready = false;
    nfc_lock();
    board_sd_supply(false);
    tick_wait(100);
    board_sd_supply(true);
    tick_wait(200);
    nfc_unlock();
    tick_wait(1000);
    clock_khz = SDMMC_FREQ_DEFAULT;
    debug_printf("sd: bus recovery\n");
    return sd_identify();
}

bool sd_start(void)
{
    if (sd_identify())
    {
        return true;
    }
    uint32_t failed_at = tick_ms();
    debug_printf("sd: identification at start failed\n");
    for (;;)
    {
        if (tick_ms() - failed_at >= 20000u)
        {
            panic_from(ACT_SD, PANIC_SD_START, 0, "no card");
        }
        monitor_feed();
        if (sd_recover())
        {
            return true;
        }
    }
}

bool sd_ready(void)
{
    return ready;
}

uint32_t sd_sectors(void)
{
    return ready ? (uint32_t)card.csd.capacity : 0;
}

void sd_set_system_end(uint32_t first_data_sector)
{
    system_end = first_data_sector;
}

static bool sd_access(bool write, uint32_t sector, uint8_t *buf, uint32_t count)
{
    if (!ready || count == 0)
    {
        return false;
    }
    for (int attempt = 0;; attempt++)
    {
        monitor_sd_transfer();
        esp_err_t e = write ? sdmmc_write_sectors(&card, buf, sector, count)
                            : sdmmc_read_sectors(&card, buf, sector, count);
        if (e == ESP_OK)
        {
            return true;
        }
        debug_printf("sd: %s of %lu sectors at %lu: %d (attempt %d)\n", write ? "write" : "read",
                     (unsigned long)count, (unsigned long)sector, e, attempt + 1);
        if (attempt == 1)
        {
            if (sector < system_end)
            {
                if (e == ESP_ERR_TIMEOUT)
                {
                    journal_write_sector(sector);
                }
                panic_from(ACT_SD, PANIC_SD_SYSTEM, sector, "SD system area failed twice");
            }
            return false;
        }
        if (!sd_recover())
        {
            panic_from(ACT_SD, PANIC_SD_RECOVERY, sector, "SD bus recovery failed");
        }
    }
}

bool sd_read(uint32_t sector, uint8_t *buf, uint32_t count)
{
    return sd_access(false, sector, buf, count);
}

bool sd_write(uint32_t sector, const uint8_t *buf, uint32_t count)
{
    return sd_access(true, sector, (uint8_t *)buf, count);
}

/* Erases released sectors; a failed erase is ignored. */
void sd_erase(uint32_t first, uint32_t last)
{
    if (!ready || last < first)
    {
        return;
    }
    monitor_sd_transfer();
    esp_err_t e = sdmmc_erase_sectors(&card, first, last - first + 1, SDMMC_ERASE_ARG);
    if (e != ESP_OK)
    {
        debug_printf("sd: erase %lu..%lu failed (%d)\n", (unsigned long)first, (unsigned long)last, e);
    }
}
