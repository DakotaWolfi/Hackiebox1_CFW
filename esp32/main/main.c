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
#include "rtnl.h"
#include "sd.h"
#include "sound.h"
#include "tag.h"
#include "tick.h"
#include "update.h"
#include "version.h"
#include "watchdog.h"

#define STACK_OP 544

static StackType_t op_stack[TASK_STACK(STACK_OP)];
static StaticTask_t op_tcb;

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
    ledui_start();
    input_start();
}
