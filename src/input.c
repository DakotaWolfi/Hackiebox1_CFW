/*
 * Input evaluation: debouncing of the ears and the charging-
 * station input once per tick, input passes, ear presses, volume steps and the
 * one-ear hold, the both-ears gestures with the
 * upside-down state, tilt
 * and slap.
 *
 * Also the extension's locks, the restart gesture and the ears while an
 * event tag stands, switching off with both ears.
 *
 * The factory-reset request restarts only (no factory reset; the setup
 * page deletes networks, contents and server settings instead).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "port.h"
#include "accel.h"
#include "board.h"
#include "debug.h"
#include "input.h"
#include "led.h"
#include "monitor.h"
#include "net.h"
#include "ledui.h"
#include "player.h"
#include "power.h"
#include "rtnl.h"
#include "settings.h"
#include "sound.h"
#include "tick.h"

enum
{
    IN_BIG,
    IN_SMALL,
    IN_STATION,
    IN_COUNT
};
enum
{
    EV_PRESS,
    EV_RELEASE,
    EV_STATION,
    EV_ACCEL
};

typedef struct
{
    uint8_t kind, which;
    uint32_t at;
} in_event_t;

static StaticQueue_t queue_mem;
static uint8_t queue_buf[16 * sizeof(in_event_t)];
static QueueHandle_t queue;
static volatile bool running;

/* --- debouncing in the tick interrupt --- */

static struct
{
    bool accepted;
    uint16_t count;
} deb[IN_COUNT];
static const PORT_ISR_DATA uint16_t deb_ticks[IN_COUNT] = {31, 31, 1001};
static bool accel_line;
static volatile uint16_t lost; /* events not queued (queue full) */

static PORT_ISR_CODE bool send_from_isr(uint8_t kind, uint8_t which, uint32_t at, BaseType_t *woken)
{
    in_event_t e = {kind, which, at};
    if (xQueueSendFromISR(queue, &e, woken) == pdTRUE)
    {
        return true;
    }
    lost++;
    return false;
}

/* The levels are sampled once per tick; a level that differs from the
 * accepted state for the full count becomes the accepted state, any sample
 * equal to it restarts the count. A pulse between two samples is not seen,
 * which a level recorded at each edge would only count toward the same
 * filter. */
void PORT_ISR_CODE vApplicationTickHook(void)
{
    if (!running)
    {
        return;
    }
    BaseType_t woken = pdFALSE;
    uint32_t now = xTaskGetTickCountFromISR();
    bool raw[IN_COUNT] = {board_ear_big_pressed(), board_ear_small_pressed(), board_on_station()};
    for (unsigned i = 0; i < IN_COUNT; i++)
    {
        if (raw[i] == deb[i].accepted)
        {
            deb[i].count = 0;
        }
        else if (++deb[i].count >= deb_ticks[i])
        {
            /* With the queue full the change is taken at a later tick, so
             * that presses and releases stay paired. */
            deb[i].count = deb_ticks[i];
            bool sent = i == IN_STATION ? send_from_isr(EV_STATION, raw[i], now, &woken)
                                        : send_from_isr(raw[i] ? EV_PRESS : EV_RELEASE, (uint8_t)i, now, &woken);
            if (sent)
            {
                deb[i].accepted = raw[i];
                deb[i].count = 0;
            }
        }
    }
    /* only a rising edge notes the time and is an event. */
    bool a = board_accel_int();
    if (a && !accel_line && !send_from_isr(EV_ACCEL, 0, now, &woken))
    {
        a = false; /* the edge again at the next tick */
    }
    accel_line = a;
    portYIELD_FROM_ISR(woken);
}

/* --- input evaluation --- */

typedef struct
{
    bool down, overlapped, synced;
    bool deferred; /* volume lock: the activity waits for a hold */
    uint32_t at;
} ear_t;
static ear_t ears[2];

typedef enum
{
    STAGE_NONE,
    STAGE_5S,
    STAGE_10S
} stage_t;
static stage_t stage;
static uint32_t both_at;
static bool upside_down; /* no after every start */
static uint32_t extra_wait;

/* network shutdown, 100 ms, reset. */
static void restart_action(void)
{
    sound_chime(CHIME_SIGNAL);
    vTaskDelay(1000);
    ledui_request(LED_POWER_ACTION);
    net_shutdown();
    vTaskDelay(100);
    led_set(0, 0, 0);
    port_restart();
}

/* The activity signal and the idle timer of an ear press. */
static void ear_activity(ear_t *e)
{
    e->deferred = false;
    power_ear_press();
    ledui_request(LED_ACTIVITY);
}

static void press(unsigned i, uint32_t at)
{
    ear_t *e = &ears[i], *o = &ears[!i];
    e->down = true;
    e->at = at;
    e->synced = false;
    e->overlapped = false;
    /* a short press stopped by the volume lock has no effect;
     * holds and both-ears gestures keep theirs, so the activity of the
     * press waits until one of them is recognised. */
    if (!o->down && !power_shutting_down() && player_ears() == EARS_NORMAL &&
        player_lock(LOCK_VOLUME, NULL) == 1)
    {
        e->deferred = true;
    }
    else
    {
        ear_activity(e);
    }
    if (o->down)
    {
        if (o->deferred)
        {
            ear_activity(o);
        }
        e->overlapped = o->overlapped = true;
        both_at = at;
        stage = STAGE_NONE;
    }
}

static void release(unsigned i, uint32_t at)
{
    ear_t *e = &ears[i], *o = &ears[!i];
    e->down = false;
    /* F9: the first release of a both-ears hold, less than
     * 2000 ms after the first of the two presses */
    if (e->overlapped && o->down)
    {
        uint32_t d = at - ((int32_t)(e->at - o->at) < 0 ? e->at : o->at);
        debug_printf("in: both %lu ms%s\n", (unsigned long)d, d < 2000 ? " -> F9" : "");
        if (d < 2000)
        {
            player_switch_off();
        }
    }
    if (e->overlapped || e->synced)
    {
        return;
    }
    if (e->deferred)
    {
        unsigned source;
        e->deferred = false;
        player_lock(LOCK_VOLUME, &source);
        player_locked(i == IN_BIG ? 5 : 6, source ? source : 2);
        return;
    }
    if (player_ears() == EARS_LED)
    {
        player_ear_level(i == IN_BIG); /* no volume step, no ear record */
        return;
    }
    if (i == IN_BIG)
    {
        sound_ear_big();
        log_event(EVT_BIG_EAR, NULL);
    }
    else
    {
        sound_ear_small();
        log_event(EVT_SMALL_EAR, NULL);
    }
}

/* Ear hold check, at the end of every pass. */
static void hold_check(void)
{
    uint32_t now = tick_ms();
    for (unsigned i = 0; i < 2; i++)
    {
        ear_t *e = &ears[i];
        if (e->down && !e->overlapped && !e->synced && now - e->at > 3000)
        {
            e->synced = true;
            if (e->deferred)
            {
                ear_activity(e);
            }
            if (player_ears_combined())
            {
                /* the hold switches between volume and LED level */
                debug_printf("in: %s ear 3 s: ears switched\n", i == IN_BIG ? "big" : "small");
                player_ears_switch();
                continue;
            }
            debug_printf("in: %s ear 3 s: sync (%u)\n", i == IN_BIG ? "big" : "small", i == IN_BIG ? 691 : 689);
            if (!net_sync_gesture()) /* setup search or setup mode take it */
            {
                player_sync_request();
            }
        }
    }
}

/* Both-ears gestures, in timed passes while both ears are pressed. */
static void both_ears_check(void)
{
    if (!ears[IN_BIG].down || !ears[IN_SMALL].down)
    {
        return;
    }
    int16_t xyz[3] = {0, 0, 0};
    if (!accel_active() || !accel_orientation(xyz))
    {
        upside_down = false;
    }
    else if (upside_down && xyz[0] < -600)
    {
        upside_down = false;
    }
    else if (!upside_down && xyz[0] > 600)
    {
        upside_down = true;
    }

    uint32_t held = tick_ms() - both_at;
    debug_verbose("in: both %lu ms, x %d up %d\n", (unsigned long)held, xyz[0], upside_down);
    if (stage == STAGE_NONE && held > 5000)
    {
        stage = STAGE_5S;
        debug_printf("in: both 5 s, x %d%s\n", xyz[0], upside_down ? " upside down" : ": setup (423)");
        if (!upside_down)
        {
            net_setup_request();
        }
    }
    else if (stage == STAGE_5S && held > 10000 && upside_down)
    {
        stage = STAGE_10S;
        /* direct read. */
        debug_printf("in: both 10 s up: %s\n",
                     board_on_station() ? "factory reset (421), restart" : "restart (422)");
        restart_action();
    }
}

static void tilt_check(void)
{
    static accel_dir_t last = DIR_NONE;
    static uint32_t printed;
    int16_t xyz[3];
    if (!accel_active())
    {
        return; /* not configured yet, or no sensor */
    }
    if (!accel_orientation(xyz))
    {
        debug_printf("in: orientation failed (7255)\n");
        return;
    }
    accel_dir_t t = accel_tilt(xyz);
    uint32_t now = tick_ms();
    if (t != last || now - printed >= 10000)
    {
        printed = now;
        debug_verbose("orientation %d %d %d, tilt %s\n", xyz[0], xyz[1], xyz[2],
                      t == DIR_A ? "A (back)" : t == DIR_B ? "B (forward)"
                                                           : "none");
    }
    last = t;
    static accel_dir_t locked_dir = DIR_NONE;
    if (t != locked_dir)
    {
        locked_dir = DIR_NONE; /* a pass without that tilt ends the sequence */
    }
    if (t == DIR_NONE)
    {
        return;
    }
    if (net_handing_over())
    {
        debug_printf("in: tilt dropped, hand-over\n");
        return;
    }
    unsigned source;
    int lock = player_lock(LOCK_TILT, &source);
    if (lock == 1)
    {
        /* one record per held tilt */
        if (locked_dir != t)
        {
            player_locked(t == DIR_B ? 3 : 4, source);
        }
        locked_dir = t;
        return;
    }
    if (lock < 0 && !settings.slap_enable)
    {
        debug_printf("in: tilt off (7063)\n");
        return;
    }
    ledui_request(LED_ACTIVITY);
    log_event(t == DIR_B ? EVT_TILT_FORWARD : EVT_TILT_BACKWARD, NULL);
    if (player_playing())
    {
        debug_printf("in: tilt %s\n", t == DIR_B ? "forward" : "back");
        player_wind(t == DIR_B);
        /* The wind chime delays the next timed pass. */
        extra_wait = 310;
    }
}

static void slap(uint32_t at)
{
    if (!accel_active())
    {
        return;
    }
    if (accel_sensor() == ACCEL_LIS3DH)
    {
        int32_t left = (int32_t)(at + 40 - tick_ms());
        if (left > 0)
        {
            vTaskDelay((TickType_t)left);
        }
    }
    accel_dir_t d = accel_slap();
    /* the headphone check follows the slap capture; it is
     * awaited so that a plug-in jolt finds the new headphone time. */
    if (!sound_check_headset_sync(500))
    {
        debug_printf("in: headset check busy\n");
    }
    if (d == DIR_NONE)
    {
        return;
    }
    if (net_handing_over())
    {
        debug_printf("in: slap dropped, hand-over\n");
        return;
    }
    bool forward = (d == DIR_A) == (settings.slap_direction == 1);
    unsigned source;
    int lock = player_lock(LOCK_SLAP, &source);
    if (lock < 0 && !settings.slap_enable)
    {
        /* a lock record replaces slap enable */
        debug_printf("in: slap off (6308)\n");
    }
    else if (tick_ms() - sound_headphone_ref() < 2000)
    {
        debug_printf("in: slap dropped, headphones (6309)\n");
    }
    else
    {
        /* the upside-down box restarts the content */
        int16_t xyz[3];
        bool up = accel_orientation(xyz) && xyz[0] > 600;
        if (lock == 1)
        {
            player_locked(up ? 7 : forward ? 1
                                           : 2,
                          source);
            return;
        }
        debug_printf("in: slap %s%s%s\n", forward ? "forward" : "back", up ? ", upside down" : "", player_playing() ? "" : ", idle");
        ledui_request(LED_ACTIVITY);
        log_event(forward ? EVT_SLAP_FORWARD : EVT_SLAP_BACKWARD, NULL);
        if (player_playing())
        {
            if (up)
            {
                player_restart_gesture(at);
            }
            else
            {
                player_chapter(forward);
            }
        }
    }
}

static void task(void *arg)
{
    (void)arg;
    running = true;
    uint32_t deadline = tick_ms() + 1000;
    for (;;)
    {
        int32_t left = (int32_t)(deadline - tick_ms());
        in_event_t ev;
        bool down = power_shutting_down();
        if (xQueueReceive(queue, &ev, left > 0 ? (TickType_t)left : 0) != pdTRUE)
        {
            /* Timed pass; while shutting down only the
             * headset read remains. */
            if (!down)
            {
                tilt_check();
                both_ears_check();
            }
            sound_check_headset();
        }
        else if (down && !(ev.kind == EV_PRESS || (ev.kind == EV_STATION && ev.which)))
        {
            /* discarded while shutting down, except presses and docking */
        }
        else
        {
            switch (ev.kind)
            {
            case EV_PRESS:
                press(ev.which, ev.at);
                break;
            case EV_RELEASE:
                release(ev.which, ev.at);
                break;
            case EV_STATION:
                debug_printf("in: station %s\n", ev.which ? "on" : "off");
                power_station_event(ev.which != 0);
                break;
            case EV_ACCEL:
                slap(ev.at);
                break;
            }
        }
        if (!down)
        {
            hold_check();
        }
        if (lost)
        {
            debug_printf("in: queue full (%u)\n", lost);
            lost = 0;
        }
        monitor_mark(ACT_INPUT);
        deadline = tick_ms() + 1000 + extra_wait;
        extra_wait = 0;
    }
}

void input_start(void)
{
    static StackType_t stack[TASK_STACK(384)];
    static StaticTask_t tcb;
    queue = xQueueCreateStatic(16, sizeof(in_event_t), queue_buf, &queue_mem);
    xTaskCreateStatic(task, "input", TASK_STACK(384), NULL, 4, stack, &tcb);
}
