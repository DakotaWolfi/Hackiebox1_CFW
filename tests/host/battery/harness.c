/* Host test: battery rules and the
 * battery report of power.c, with the other modules stubbed.
 * usage: battery_test
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#define xTaskNotifyGive(t) ((void)(t))
#include "power.c"
#include "pwrstate.c"

static int failures;
#define CHECK(c, ...)                       \
    do                                      \
    {                                       \
        if (!(c))                           \
        {                                   \
            failures++;                     \
            printf("  FAIL: " __VA_ARGS__); \
            printf("\n");                   \
        }                                   \
        else                                \
        {                                   \
            printf("  ok: " __VA_ARGS__);   \
            printf("\n");                   \
        }                                   \
    } while (0)

/* --- time, board, battery --- */
static uint32_t sim_ms = 1000;
uint32_t tick_ms(void) { return sim_ms; }
void vTaskDelay(TickType_t t) { sim_ms += t; }
static bool station;
static uint32_t battery_mv = 3900;
bool board_on_station(void) { return station; }
bool board_ear_big_pressed(void) { return false; }
bool board_ear_small_pressed(void) { return false; }
uint32_t battery_measure(void) { return battery_mv; }
uint32_t battery_last_raw(void) { return battery_mv / 4; }
uint32_t battery_wake_measure(void) { return battery_mv; }
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("    | ");
    vprintf(f, a);
    va_end(a);
}

/* --- records --- */
static uint32_t rec_args[16][8];
static unsigned rec_n, rec_len[16];
void log_arg_num(log_args_t *a, int32_t v)
{
    memcpy(a->data + a->n, &v, 4);
    a->n += 4;
}
void log_fn(uint8_t level, uint16_t group, uint16_t function, uint8_t variant, const log_args_t *a)
{
    (void)variant;
    if (level != LOG_INFO || group != 22 || function != 32262 || rec_n >= 16)
    {
        return;
    }
    rec_len[rec_n] = a->n;
    memcpy(rec_args[rec_n], a->data, a->n);
    rec_n++;
}
void log_event(log_event_t type, const uint8_t *uid)
{
    (void)type;
    (void)uid;
}
void log_drain(void) {}
static bool answered;
bool ext_hello_answered(void) { return answered; }

/* --- player, LED, sound --- */
static int prompts[8], prompt_n;
static bool busy, low_led;
void player_prompt(unsigned nn, bool wait)
{
    (void)wait;
    prompts[prompt_n++ % 8] = (int)nn;
}
bool player_busy(void) { return busy; }
void player_status(player_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->state = busy ? 2 : 0;
    s->step = 6;
}
void player_timer_steps(uint8_t step[2]) { (void)step; }
void ledui_request(led_request_t r) { (void)r; }
void ledui_low_battery(bool on) { low_led = on; }
void sound_chime(chime_t c) { (void)c; }
bool sound_park(uint32_t max_ms)
{
    (void)max_ms;
    return true;
}
void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
}

/* --- system --- */
settings_t settings;
static jmp_buf off;
static bool went_off;
void net_shutdown(void) {}
uint8_t net_state(void) { return 6; }
void monitor_power_activity(bool b) { (void)b; }
void monitor_feed(void) {}
bool monitor_in_update_round(void) { return false; }
void panic_power_phase(power_phase_t p) { (void)p; }
unsigned start_reason(void) { return 0; }
bool retained_state(uint32_t rec[2])
{
    (void)rec;
    return false;
}
void retained_store_state(uint32_t w1, uint32_t w2)
{
    (void)w1;
    (void)w2;
}
bool bootstate_trial_pending(void) { return false; }
void off_state_enter(void)
{
    went_off = true;
    longjmp(off, 1);
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 1000)
    {
        sim_ms += 1000;
        power_check();
    }
}

int main(void)
{
    printf("B1 start before the hello: no report; at the hello the last measurement (reason 0)\n");
    power_start(NULL);
    run(5000);
    CHECK(rec_n == 0, "nothing before the hello answer");
    answered = true;
    run(1000);
    CHECK(rec_n == 1 && rec_len[0] == 20 && rec_args[0][0] == 3900 && rec_args[0][2] == 0 && rec_args[0][4] == 975,
          "reason 0: 3900 mV, ADC value 975");

    printf("B2 scheduled measurement while playing, about every 121 s (reason 1)\n");
    busy = true;
    run(110000);
    CHECK(rec_n == 1, "none before 121 s");
    run(10000);
    CHECK(rec_n == 2 && rec_args[1][2] == 1 && rec_args[1][1] == 0 && rec_args[1][3] == (2u | 6u << 8),
          "reason 1, no flags, load: playing, step 6");

    printf("B3 low battery: prompt 3, orange, flag bit 1\n");
    battery_mv = 3650;
    run(122000);
    CHECK(prompt_n == 1 && prompts[0] == 3 && low_led, "prompt 3, ready colour orange");
    CHECK(rec_n == 3 && rec_args[2][0] == 3650 && rec_args[2][1] == 2, "reported with the low-battery state");

    printf("B4 off the station: removal measurement (reason 2), then a measurement (reason 1)\n");
    unsigned before = rec_n;
    station = true;
    power_station_event(true);
    run(1000);
    station = false;
    power_station_event(false);
    run(1000);
    CHECK(rec_n == before + 3 && rec_args[before][2] == 1 && rec_args[before + 1][2] == 2 && (rec_args[before + 1][1] & 1) == 0 &&
              rec_args[before + 2][2] == 1,
          "after docking reason 1 on the station, then reason 2 and reason 1 off it");
    CHECK((rec_args[before][1] & 1) == 1, "on the station: flag bit 0");

    printf("B5 implausible value: flag bit 3, no rule applied\n");
    battery_mv = 2000;
    prompt_n = 0;
    run(122000);
    CHECK(rec_n == before + 4 && (rec_args[before + 3][1] & 8) && prompt_n == 0, "reported as implausible, no prompt");

    printf("B6 critical battery: prompt 9, off after it, content not started again\n");
    battery_mv = 3590;
    if (!setjmp(off))
    {
        run(122000);
        CHECK(prompt_n == 1 && prompts[0] == 9 && power_critical_pending(), "prompt 9, power-off pending");
        CHECK((rec_args[rec_n - 1][1] & 4) != 0, "reported with the critical flag");
        run(1000); /* prompt 9 still playing */
        CHECK(!went_off, "not while the prompt plays");
        busy = false;
        run(3000);
    }
    CHECK(went_off, "off once the prompt has ended");
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
