/*
 * Battery rules and charging-station handling: measurement
 * schedule, low-battery warning and recovery, critical battery, station events and their
 * handling, the battery report to the
 * extension server.
 *
 * The idle power-off, also on request of the
 * extension, the power-off of a critical battery, the power-off and restart
 * sequences, the retained state and the check
 * at wake-up. Without CFW_HIBERNATE the off state is a wait
 * with LED dark, audio parked and network down that an ear press or the
 * station ends with an MCU reset; its state record is then restored as
 * after a hibernation exit (pwrstate.h).
 *
 * Not yet: the deferral while a sync runs, the announcement
 * queue's activity rule, the slow-clock check,
 * receive statistics and network processor clock (power-off steps 2, 4),
 * stack records.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "port.h"
#include "battery.h"
#include "board.h"
#include "bootstate.h"
#include "debug.h"
#include "ext.h"
#include "led.h"
#include "ledui.h"
#include "monitor.h"
#include "net.h"
#include "panic.h"
#include "player.h"
#include "power.h"
#include "pwrstate.h"
#include "rtnl.h"
#include "settings.h"
#include "sound.h"
#include "tick.h"
#include "watchdog.h"

static TaskHandle_t checker;
static volatile bool station_pending, station_state;
static bool station_handled;
static bool measure_next, low_battery, critical_announced;
static uint32_t last_measure;
static volatile uint32_t idle_since;
static bool critical_pending, was_busy;
static uint32_t critical_at, off_soon_at;
static volatile bool shutting_down, restart_requested, step4_begun, off_soon;

/* Hooks of other modules; these weak defaults hold until they are wired
 * (power.h). */
__attribute__((weak)) bool player_idle(void)
{
    return !player_busy();
}
__attribute__((weak)) void player_power_off(void) {}
__attribute__((weak)) void sound_volume_get(uint8_t step[2])
{
    step[0] = step[1] = 8;
}
__attribute__((weak)) void sound_volume_set(const uint8_t step[2])
{
    (void)step;
}
__attribute__((weak)) uint32_t player_resume_get(void)
{
    return 0;
}
__attribute__((weak)) void player_resume_set(uint32_t r)
{
    (void)r;
}
__attribute__((weak)) bool net_offline(void)
{
    return false;
}
__attribute__((weak)) void net_offline_set(bool on)
{
    (void)on;
}

static void off_checks(void);

/* --- battery report --- */

static bool reported_at_hello, last_plausible;
static uint32_t last_mv, last_adc;
static bool last_station;

static void report(uint32_t mv, uint32_t adc, bool station, bool ok, uint32_t reason)
{
    player_status_t p;
    player_status(&p);
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)mv);
    log_arg_num(&a, (int32_t)((station ? 1u : 0) | (low_battery ? 2u : 0) | (critical_pending ? 4u : 0) | (ok ? 0 : 8u)));
    log_arg_num(&a, (int32_t)reason);
    log_arg_num(&a, (int32_t)(p.state | (uint32_t)p.step << 8 | (uint32_t)(p.headphones & 1) << 16));
    log_arg_num(&a, (int32_t)adc);
    log_fn(LOG_INFO, 22, 32262, 0, &a);
}

/* A measurement: reported when the hello answer is out, else kept for the
 * report right after it. */
static void report_measurement(uint32_t mv, bool ok, uint32_t reason)
{
    if (reason == 1)
    {
        last_mv = mv;
        last_adc = battery_last_raw();
        last_station = board_on_station();
        last_plausible = ok;
    }
    if (ext_hello_answered() && reported_at_hello)
    {
        report(mv, battery_last_raw(), board_on_station(), ok, reason);
    }
}

static void report_at_hello(void)
{
    if (!reported_at_hello && ext_hello_answered())
    {
        reported_at_hello = true;
        report(last_mv, last_adc, last_station, last_plausible, 0);
    }
}

static void evaluate(uint32_t mv)
{
    if (mv <= 3600)
    {
        if (critical_announced)
        {
            debug_printf("pwr: crit again\n");
            return;
        }
        critical_announced = true;
        critical_pending = true;
        critical_at = tick_ms();
        debug_printf("pwr: crit %lumV raw %lu -> off\n", (unsigned long)mv, (unsigned long)battery_last_raw());
        ledui_request(LED_BATTERY_EMPTY);
        player_prompt(9, false);
        return;
    }
    if (mv <= 3669 && !low_battery)
    {
        low_battery = true;
        debug_printf("pwr: low\n");
        ledui_low_battery(true);
        ledui_request(LED_BATTERY_LOW);
        player_prompt(3, false);
    }
    else if (mv > 3700 && low_battery)
    {
        low_battery = false;
        debug_printf("pwr: low over\n");
        ledui_low_battery(false);
    }
}

/* Deliberate deviation from the battery rules until the ADC
 * conversion is confirmed on the box: a running box cannot be below
 * 3000 mV or, off the station, above 4500 mV, so such a reading (wrong
 * conversion, no ADC word) is logged and the battery rules are not applied
 * to it. On the station the measured voltage rises with the charger (4741
 * mV seen on the box, 2026-10-12): up to 6000 mV is accepted there. */
static bool plausible(uint32_t mv)
{
    return mv >= 3000 && mv <= (board_on_station() ? 6000u : 4500u);
}

static void measure(void)
{
    uint32_t mv = battery_measure();
    last_measure = tick_ms();
    measure_next = false;
    bool ok = plausible(mv);
    debug_printf("bat %lumV raw %lu st %u%s\n", (unsigned long)mv, (unsigned long)battery_last_raw(),
                 board_on_station(), ok ? "" : " implausible, ignored");
    if (ok)
    {
        evaluate(mv);
    }
    report_measurement(mv, ok, 1);
}

void power_start(void *task)
{
    checker = (TaskHandle_t)task;
    /* the state handled last is the raw level at start. */
    station_handled = board_on_station();
    idle_since = tick_ms();
    measure();
}

void power_activity(void)
{
    idle_since = tick_ms();
}

/* an ear press or docking during the idle power-off asks for
 * a restart, if it comes before step 4. */
static void interrupt_request(void)
{
    if (!shutting_down || restart_requested)
    {
        return;
    }
    if (step4_begun)
    {
        debug_printf("pwr: restart req late\n");
        return;
    }
    restart_requested = true;
    panic_power_phase(PHASE_RESTARTING);
    debug_printf("pwr: off broken, restart\n");
}

bool power_shutting_down(void)
{
    return shutting_down;
}

bool power_critical_pending(void)
{
    return critical_pending;
}

/* not before 500 ms, so that the lilac blink of
 * F9 is shown in full before the LED goes dark. */
void power_off_soon(void)
{
    off_soon_at = tick_ms();
    off_soon = true;
}

void power_ear_press(void)
{
    power_activity();
    interrupt_request();
}

void power_station_event(bool on)
{
    power_activity();
    if (on)
    {
        interrupt_request();
    }
    if (shutting_down)
    {
        return; /* station handling does not run */
    }
    log_event(on ? EVT_CHARGER_ON : EVT_CHARGER_OFF, NULL);
    CRITICAL_ENTER();
    bool was_pending = station_pending;
    station_state = on;
    station_pending = true;
    CRITICAL_EXIT();
    if (!was_pending)
    {
        ledui_request(LED_ACTIVITY);
    }
    if (checker)
    {
        xTaskNotifyGive(checker);
    }
}

static void station_handling(bool on)
{
    if (on == station_handled)
    {
        return;
    }
    station_handled = on;
    if (on)
    {
        sound_chime(CHIME_CHARGING_STATION);
    }
    else
    {
        uint32_t mv = battery_measure();
        debug_printf("bat %lumV raw %lu off station\n", (unsigned long)mv, (unsigned long)battery_last_raw());
        report_measurement(mv, plausible(mv), 2);
        if (mv > 3299 || !plausible(mv))
        {
            ledui_request(LED_STATION_REMOVED);
        }
        else
        {
            ledui_request(LED_STATION_REMOVED_LOW);
            player_prompt(13, false);
        }
        /* The re-check: a change meanwhile has set the
         * pending flag again, so the next check handles it. */
    }
    measure_next = true;
}

void power_check(void)
{
    CRITICAL_ENTER();
    bool pending = station_pending;
    bool on = station_state;
    station_pending = false;
    CRITICAL_EXIT();
    report_at_hello();
    if (pending)
    {
        station_handling(on);
    }
    /* more than 120 whole seconds. */
    if (measure_next || (tick_ms() - last_measure) / 1000 > 120)
    {
        measure();
    }
    off_checks();
}

/* --- retained state --- */

static bool restored_station;

static void gather(pwrstate_t *p)
{
    uint8_t step[2];
    sound_volume_get(step);
    player_timer_steps(step); /* also at a critical battery */
    p->limit_speaker = settings.limit_speaker;
    p->limit_headphones = settings.limit_headphones;
    p->step_speaker = step[0];
    p->step_headphones = step[1];
    p->slap_enable = settings.slap_enable;
    p->slap_direction_1 = settings.slap_direction == 1;
    p->field6 = settings.reserved6 != 0;
    p->led = settings.led;
    p->offline = net_offline();
    p->on_station = board_on_station(); /* raw input at this moment */
    p->resume = player_resume_get();
}

static void save(const pwrstate_t *p, bool soft_off)
{
    uint32_t w[2];
    pwrstate_encode(p, soft_off, w);
    retained_store_state(w[0], w[1]);
    debug_verbose("pwr: saved %08lx %08lx\n", (unsigned long)w[0], (unsigned long)w[1]);
}

void power_restore(void)
{
    uint32_t w[2];
    pwrstate_t p;
    if (!retained_state(w) || !pwrstate_decode(w, &p))
    {
        return; /* defaults */
    }
    settings.limit_speaker = p.limit_speaker;
    settings.limit_headphones = p.limit_headphones;
    settings.slap_enable = p.slap_enable;
    settings.slap_direction = p.slap_direction_1 ? 1 : 0;
    settings.reserved6 = p.field6;
    settings.led = p.led;
    uint8_t step[2] = {p.step_speaker, p.step_headphones};
    sound_volume_set(step);
    net_offline_set(p.offline);
    /* a test start discards the resume point. */
    player_resume_set(bootstate_trial_pending() ? 0 : p.resume);
    restored_station = p.on_station;
    debug_verbose("pwr: restored steps %u/%u st %u off %u led %u\n", p.step_speaker, p.step_headphones, p.on_station,
                  p.offline, p.led);
}

/* --- off state --- */

static void __attribute__((noreturn)) enter_off(const pwrstate_t *p)
{
    save(p, !CFW_HIBERNATE);
    led_set(0, 0, 0);
    off_state_enter(); /* without CFW_HIBERNATE a polled wait, then an MCU reset */
}

/* Power-off, from the parking of the audio output on. */
static void __attribute__((noreturn)) power_off_sequence(void)
{
    sound_park(500);
    net_shutdown();
    pwrstate_t p;
    gather(&p);
    enter_off(&p);
}

static void __attribute__((noreturn)) restart_after_interruption(void)
{
    sound_park(500);
    net_shutdown();
    pwrstate_t p;
    gather(&p);
    save(&p, false); /* a reset, not a hibernation exit: defaults follow */
    debug_printf("pwr: restart\n");
    vTaskDelay(pdMS_TO_TICKS(100) + 1);
    led_set(0, 0, 0);
    port_restart();
}

static void __attribute__((noreturn)) idle_power_off(void)
{
    debug_printf("pwr: idle off (%lu s)\n", (unsigned long)((tick_ms() - idle_since) / 1000));
    shutting_down = true;
    panic_power_phase(PHASE_PREPARING_OFF);
    monitor_power_activity(true);
    ledui_request(LED_OFF);
    player_power_off(); /* step 3: stop, drop requests, housekeeping */
    monitor_feed();
    step4_begun = true; /* step 4 */
    if (restart_requested)
    {
        restart_after_interruption();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    monitor_feed();
    if (net_state() == 6)
    {
        log_drain(); /* step 5 */
    }
    power_off_sequence(); /* step 6 */
}

/* from power_check(), activity 1. */
static void off_checks(void)
{
    uint32_t now = tick_ms();
    bool idle = player_idle();
    if (critical_pending)
    {
        /* After prompt 9 has finished; at most 60 s. */
        if ((now - critical_at > 2000 && idle) || now - critical_at > 60000)
        {
            debug_printf("pwr: crit off\n");
            monitor_power_activity(true);
            panic_power_phase(PHASE_PREPARING_OFF);
            power_off_sequence();
        }
        return;
    }
    if (!idle)
    {
        was_busy = true;
        return;
    }
    if (was_busy)
    {
        was_busy = false;
        power_activity(); /* the box became idle */
    }
    bool soon = off_soon && (int32_t)(now - off_soon_at) >= 500; /* power_off_soon() */
    if (!monitor_in_update_round() && (soon || (now - idle_since) / 1000 > 600))
    {
        idle_power_off();
    }
}

/* --- wake-up check --- */

void power_wake_check(void)
{
#if CFW_HIBERNATE
    if (start_reason() != START_HIB_EXIT || !restored_station || board_on_station())
    {
        return;
    }
    uint32_t mv = battery_wake_measure();
    debug_printf("pwr: off station, %lu mV\n", (unsigned long)mv);
    bool green = mv > 3299;
    for (int phase = 0; phase < 10; phase++)
    {
        if (phase % 2)
        {
            led_set(0, 0, 0);
        }
        else if (green)
        {
            led_set(0, 255, 0);
        }
        else
        {
            led_set(170, 48, 0);
        }
        for (int i = 0; i < 100; i++)
        {
            if (board_on_station() || board_ear_big_pressed() || board_ear_small_pressed())
            {
                led_set(0, 0, 0);
                debug_printf("pwr: input, start\n");
                return;
            }
        }
    }
    pwrstate_t p;
    gather(&p);
    p.on_station = false;
    p.resume = 0;
    enter_off(&p);
#endif
}
