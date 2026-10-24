/*
 * Start-up of the ESP32 box. The order follows src/cc3200/main.c, with the
 * slot check first: the count of
 * unconfirmed starts and a rollback come before anything else that can
 * fail.
 *
 * The operating-functions activity starts Wi-Fi, bus, accelerometer and
 * codec, the card (with the server block of the image and the built-in
 * setup page), the cloud, player and tag reader, then runs the battery and
 * station checks and feeds the watchdog.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "accel.h"
#include "board.h"
#include "bootstate.h"
#include "buildblock.h"
#include "cloud.h"
#include "debug.h"
#include "esp32.h"
#include "ff.h"
#include "i2c.h"
#include "input.h"
#include "journal.h"
#include "ledui.h"
#include "monitor.h"
#include "net.h"
#include "panic.h"
#include "player.h"
#include "port.h"
#include "power.h"
#include "remote.h"
#include "rtnl.h"
#include "sd.h"
#include "sound.h"
#include "tag.h"
#include "tick.h"
#include "update.h"
#include "version.h"
#include "watchdog.h"
#include "esp_psram.h"

#define STACK_OP 544
#define STACK_SERIAL 352

static StackType_t op_stack[TASK_STACK(STACK_OP)];
static StaticTask_t op_tcb;
static StackType_t serial_stack[TASK_STACK(STACK_SERIAL)];
static StaticTask_t serial_tcb;

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_uid16(const char *s, uint8_t uid[8])
{
    if (strlen(s) != 16)
    {
        return false;
    }
    for (unsigned i = 0; i < 8; i++)
    {
        int hi = hex_nibble((unsigned char)s[2 * i]);
        int lo = hex_nibble((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        uid[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void serial_help(void)
{
    debug_printf("serial: commands: status, play <uid16>, pause, continue, stop, next, prev, vol <1..15>, setup\n");
}

static void serial_task(void *arg)
{
    (void)arg;
    char line[96];
    serial_help();
    for (;;)
    {
        if (!fgets(line, sizeof line, stdin))
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        char *nl = strpbrk(line, "\r\n");
        if (nl)
        {
            *nl = 0;
        }
        if (!line[0])
        {
            continue;
        }
        if (!strncmp(line, "play ", 5))
        {
            uint8_t uid[8];
            if (!parse_uid16(line + 5, uid))
            {
                debug_printf("serial: play expects 16 hex digits\n");
                continue;
            }
            debug_printf("serial: play result %u\n", remote_command(71, uid, sizeof uid));
        }
        else if (!strcmp(line, "pause"))
        {
            debug_printf("serial: pause result %u\n", remote_command(65, NULL, 0));
        }
        else if (!strcmp(line, "continue"))
        {
            debug_printf("serial: continue result %u\n", remote_command(66, NULL, 0));
        }
        else if (!strcmp(line, "stop"))
        {
            debug_printf("serial: stop result %u\n", remote_command(67, NULL, 0));
        }
        else if (!strcmp(line, "next"))
        {
            debug_printf("serial: next result %u\n", remote_command(68, NULL, 0));
        }
        else if (!strcmp(line, "prev"))
        {
            debug_printf("serial: prev result %u\n", remote_command(69, NULL, 0));
        }
        else if (!strncmp(line, "vol ", 4))
        {
            long v = strtol(line + 4, NULL, 10);
            uint8_t a = v >= 0 && v <= 255 ? (uint8_t)v : 255;
            debug_printf("serial: vol result %u\n", remote_command(70, &a, 1));
        }
        else if (!strcmp(line, "status"))
        {
            remote_status_t s;
            remote_status(&s);
            debug_printf("serial: state %lu uid %02x%02x%02x%02x%02x%02x%02x%02x chapter %lu pos %lu vol %lu\n",
                         (unsigned long)s.state, s.uid[0], s.uid[1], s.uid[2], s.uid[3], s.uid[4], s.uid[5], s.uid[6],
                         s.uid[7], (unsigned long)s.chapter, (unsigned long)s.position, (unsigned long)s.volume);
        }
        else if (!strcmp(line, "setup"))
        {
            net_setup_request();
            debug_printf("serial: setup requested\n");
        }
        else if (!strcmp(line, "help"))
        {
            serial_help();
        }
        else
        {
            debug_printf("serial: unknown command '%s'\n", line);
            serial_help();
        }
    }
}

static void sd_start_and_mount(void)
{
    static FATFS fs;
    uint32_t t0 = tick_ms();
    bool ok = sd_start();
    debug_printf("sd: identification %s (%lu ms), %lu sectors\n", ok ? "ok" : "FAILED",
                 (unsigned long)(tick_ms() - t0), (unsigned long)sd_sectors());
    if (!ok)
    {
        return;
    }
    FRESULT r = f_mount(&fs, "", 1);
    bootstate_report(BOOT_CHECK_CARD, r == FR_OK);
    debug_printf("sd: mount %d, FAT type %d, cluster %u sectors\n", r, fs.fs_type, fs.csize);
    if (r != FR_OK)
    {
        return;
    }
    fault_report_sd();
    journal_apply_content();
    srvblock_apply();
    page_default_install(); /* the page on the card */
}

static void op_task(void *arg)
{
    (void)arg;
    net_start();
    uint32_t t0 = tick_ms();
    i2c_init();
    accel_type_t acc = accel_identify();
    debug_printf("accelerometer: %s (%lu ms)\n", accel_name(acc), (unsigned long)(tick_ms() - t0));
    if (acc != ACCEL_NONE)
    {
        debug_printf("accelerometer configured: %s\n", accel_configure(acc) ? "ok" : "FAILED");
    }
    sound_start();
    journal_load(); /* before the mount */
    sd_start_and_mount();
    monitor_feed();
    cloud_start();
    player_start();
    tag_start();
    monitor_feed();
    power_start(xTaskGetCurrentTaskHandle());

    uint32_t next = tick_ms() + 1000;
    for (;;)
    {
        monitor_round(); /* feeds the watchdog */
        bootstate_trial_poll(net_state() >= 3);
        if (update_restart_pending())
        {
            update_restart();
        }
        power_check();
        uint32_t now = tick_ms();
        if ((int32_t)(now - next) >= 0)
        {
            next += 1000;
            if ((int32_t)(now - next) >= 0)
            {
                next = now + 1000;
            }
        }
        ulTaskNotifyTake(pdTRUE, next - tick_ms() <= 1000 ? next - tick_ms() : 0);
    }
}

void app_main(void)
{
    debug_init();
    slots_early(); /* before the first use of the namespace */
    bool wdt = watchdog_setup();
    nvsfile_init();
    slots_start(); /* may roll back at once */
    board_early_init();
    debug_printf("\n%s %s build %lu wdt %u st %u\n", CFW_VERSION_STRING, CFW_REVISION, (unsigned long)CFW_BUILD_TIME,
                 wdt, board_on_station());
    debug_printf("psram: %s, %lu bytes\n", esp_psram_is_initialized() ? "initialized" : "not initialized",
                 (unsigned long)esp_psram_get_size());
    retained_start();
    buildblock_check();
    if (!board_scl_high_at_start() || !board_sda_high_at_start())
    {
        debug_printf("i2c: lines at start SCL %d SDA %d\n", board_scl_high_at_start(), board_sda_high_at_start());
    }
    assets_mount();

    sound_init();
    log_init();
    player_init();
    cloud_init();
    power_restore();
    power_wake_check();
    xTaskCreateStatic(op_task, "op", TASK_STACK(STACK_OP), NULL, 2, op_stack, &op_tcb);
    xTaskCreateStatic(serial_task, "serial", TASK_STACK(STACK_SERIAL), NULL, 1, serial_stack, &serial_tcb);
    ledui_start();
    input_start();
}
