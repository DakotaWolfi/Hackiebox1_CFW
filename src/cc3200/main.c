/*
 * Start-up: steps 1 to 10 before the system tick, then the
 * kernel starts (step 11) and the activities take over (step 12).
 *
 * The operating-functions activity starts the network processor, the card,
 * the cloud, the sensors, sound, player and tag reader, then runs the
 * battery and station checks and feeds the watchdog; the
 * input activity (input.c) evaluates ears, station, tilt and slap.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "FreeRTOS.h"
#include "task.h"
#include "accel.h"
#include "bootstate.h"
#include "buildblock.h"
#include "journal.h"
#include "monitor.h"
#include "panic.h"
#include "audio_out.h"
#include "battery.h"
#include "board.h"
#include "cloud.h"
#include "debug.h"
#include "dma.h"
#include "handover.h"
#include "i2c.h"
#include "input.h"
#include "led.h"
#include "ledui.h"
#include "net.h"
#include "sound.h"
#include "ff.h"
#include "player.h"
#include "power.h"
#include "rtnl.h"
#include "settings.h"
#include "sd.h"
#include "tag.h"
#include "tick.h"
#include "update.h"
#include "version.h"
#include "watchdog.h"

/* Task priorities: inputs and LED above the operating functions. */
enum
{
    PRIO_OP = 2,
};

#define STACK_OP 544 /* worst case 1432 B (make stackcheck), +25 % */

static StackType_t op_stack[STACK_OP];
static StaticTask_t op_tcb;

/* Activity 1: operating functions. Feeds the watchdog. */
/* Identify and mount the card and list its root (the SD start-up
 * records follow). Nothing is written: the box creates only its own
 * names. */
static void sd_test(void)
{
    static FATFS fs;
#if CFW_DEBUG >= 2
    static DIR dir;
    static FILINFO fi;
#endif

    uint32_t t0 = tick_ms();
    bool ok = sd_start();
    debug_printf("sd: ident %s %lu ms, %lu sectors\n", ok ? "ok" : "FAILED",
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
    journal_apply_content(); /* content of a version 1 journal to BADBLOCK */
#if CFW_DEBUG >= 2
    if (f_opendir(&dir, "/") == FR_OK)
    {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0])
        {
            debug_printf("  %s%s %lu\n", fi.fname, fi.fattrib & AM_DIR ? "/" : "", (unsigned long)fi.fsize);
        }
        f_closedir(&dir);
    }
#endif
}

static void op_task(void *arg)
{
    (void)arg;
    /* The SPI to the network processor runs on uDMA channels 12 and 13
     * (cc_pal.c), which spi_Open enables only when the controller is
     * set up by then. */
    udma_init();
    /* the network processor starts early, before the settings
     * are read and before any network activity. */
    net_start();
    /* Step 12: bus, accelerometer and codec do not need the
     * network processor; they start while it comes up, so the start-up
     * prompt is not held back by the wait below. */
    uint32_t t0 = tick_ms();
    i2c_init();
    accel_type_t acc = accel_identify();
    debug_printf("accelerometer: %s (%lu ms)\n", accel_name(acc), (unsigned long)(tick_ms() - t0));
    if (acc != ACCEL_NONE)
    {
        debug_printf("accelerometer configured: %s\n", accel_configure(acc) ? "ok" : "FAILED");
    }
    sound_start();
    /* the journal before the mount. It is in the network
     * processor's file system, so only once the net activity has started
     * it and made its start-up restarts (settings_load() is their end);
     * sl_Fs* calls during sl_Start would race with the driver set-up. */
    t0 = tick_ms();
    while (!settings_ready() && tick_ms() - t0 < 60000)
    {
        monitor_feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    debug_printf("op: nwp %s %lu ms\n", settings_ready() ? "up" : "NOT up, no journal", (unsigned long)(tick_ms() - t0));
    if (settings_ready())
    {
        journal_load();
    }
    sd_test();
    monitor_feed(); /* the card start can take 20 s and more */
    cloud_start();
    player_start();
    tag_start();
    monitor_feed();
    power_start(xTaskGetCurrentTaskHandle());

    /* The power checks about once per second; the station
     * handling wakes the activity earlier. */
    uint32_t next = tick_ms() + 1000;
    for (unsigned n = 0;;)
    {
        monitor_round(); /* feeds the watchdog */
        /* joined means associated or later. */
        bootstate_trial_poll(net_state() >= 3);
        if (update_restart_pending())
        {
            update_restart(); /* not from the cloud activity */
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
            n++;
            if (n % 10 == 0)
            {
                debug_verbose("uptime %lu ms, connection state %u\n", (unsigned long)now, net_state());
            }
        }
        ulTaskNotifyTake(pdTRUE, next - tick_ms() <= 1000 ? next - tick_ms() : 0);
    }
}

int main(void)
{
    bool wdt = watchdog_setup();
    debug_init();
    board_early_init();
    /* Start banner: version, watchdog left running by the bootloader,
     * station; then the "start:" line (retained_start) and the "boot:" line
     * (buildblock_check), both before any panic of theirs. */
    debug_printf("\n%s %s build %lu wdt %u st %u\n", CFW_VERSION_STRING, CFW_REVISION,
                 (unsigned long)CFW_BUILD_TIME, wdt, board_on_station());
    retained_start();
    buildblock_check();
    if (!board_scl_high_at_start() || !board_sda_high_at_start())
    {
        /* a low line at the start-up check is reported. */
        debug_printf("i2c: start SCL %d SDA %d, pins %04lx\n", board_scl_high_at_start(),
                     board_sda_high_at_start(), (unsigned long)board_loader_pins());
    }

    sound_init();
    log_init();
    player_init();
    cloud_init();
    power_restore();
    power_wake_check(); /* CFW_HIBERNATE only; may go off again */
    xTaskCreateStatic(op_task, "op", STACK_OP, NULL, PRIO_OP, op_stack, &op_tcb);
    ledui_start();
    input_start();
    vTaskStartScheduler();
    for (;;)
        ;
}
