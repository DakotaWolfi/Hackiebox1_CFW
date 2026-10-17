/*
 * LED requests and patterns.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "port.h"
#include "led.h"
#include "ledui.h"
#include "panic.h"
#include "settings.h"
#include "tick.h"

typedef struct
{
    uint8_t r, g, b;
} rgb_t;

static const rgb_t OFF = {0, 0, 0}, GREEN = {0, 255, 0}, BLUE = {0, 0, 255}, RED = {144, 0, 0},
                   ORANGE = {170, 48, 0}, WHITE = {144, 255, 255},
                   TURQUOISE = {0, 255, 255}, LILAC = {144, 0, 255}, TURQUOISE_DIM = {0, 25, 25};

static rgb_t shown;

/* channel value to PWM level, linear between the breakpoints
 * every 32 values (the last interval ends at 255), 255 passed through. */
static uint8_t level(uint8_t v)
{
    static const uint8_t bp[9] = {0, 8, 20, 36, 56, 80, 108, 160, 255};
    if (v == 255)
    {
        return 254;
    }
    unsigned i = v >> 5, v0 = i * 32, w = i == 7 ? 31 : 32;
    return (uint8_t)(bp[i] + (unsigned)(bp[i + 1] - bp[i]) * (v - v0) / w);
}

/* `shown` keeps the channel values, so fades continue from them. */
static void show(rgb_t c)
{
    shown = c;
    if (c.r == c.g && c.g == c.b && c.r >= 112)
    {
        c.r = c.g = c.b = 112;
    }
    if (fault_active())
    {
        return; /* the fault handling owns the LED */
    }
    led_set(level(c.r), level(c.g), level(c.b));
}

static volatile bool low_battery, offline;

/* orange before white before green. */
static rgb_t ready_colour(void)
{
    return low_battery ? ORANGE : offline ? WHITE
                                          : GREEN;
}

void ledui_low_battery(bool on)
{
    if (on == low_battery)
    {
        return;
    }
    low_battery = on;
    ledui_request(LED_COLOUR_CHANGE);
}

void ledui_offline(bool on)
{
    if (on == offline)
    {
        return;
    }
    offline = on;
    ledui_request(LED_COLOUR_CHANGE);
}

static volatile int ext_level = -1;

void ledui_ext_level(int level)
{
    if (level == ext_level)
    {
        return;
    }
    ext_level = level;
    ledui_request(LED_COLOUR_CHANGE);
}

/* target colour by the brightness setting, or the extension's
 * level. */
static rgb_t ready_target(void)
{
    rgb_t c = ready_colour();
    int led = ext_level >= 0 ? ext_level : settings.led;
    if (led == 1)
    {
        return OFF;
    }
    if (led == 2)
    {
        return (rgb_t){(uint8_t)(c.r * 26 / 256), (uint8_t)(c.g * 26 / 256), (uint8_t)(c.b * 26 / 256)};
    }
    return c;
}

/* --- fade --- */

static struct
{
    rgb_t a, b;
    uint32_t d, start, interval;
} fade;

static int absdiff(int x, int y)
{
    return x > y ? x - y : y - x;
}

static void fade_start(rgb_t a, rgb_t b, uint32_t d)
{
    int m = absdiff(a.r, b.r);
    if (absdiff(a.g, b.g) > m)
    {
        m = absdiff(a.g, b.g);
    }
    if (absdiff(a.b, b.b) > m)
    {
        m = absdiff(a.b, b.b);
    }
    fade.a = a;
    fade.b = b;
    fade.d = d;
    fade.start = tick_ms();
    fade.interval = m ? (d / (uint32_t)m ? d / (uint32_t)m : 1) : 1;
    show(a);
}

static uint8_t lerp(uint8_t a, uint8_t b, uint32_t e, uint32_t d)
{
    return (uint8_t)((int)a + ((int)b - (int)a) * (int32_t)e / (int32_t)d);
}

/* One recomputation; true when the fade has ended on b. */
static bool fade_step(void)
{
    uint32_t e = tick_ms() - fade.start;
    if (e > fade.d)
    {
        e = fade.d;
    }
    rgb_t c = fade.d ? (rgb_t){lerp(fade.a.r, fade.b.r, e, fade.d), lerp(fade.a.g, fade.b.g, e, fade.d),
                               lerp(fade.a.b, fade.b.b, e, fade.d)}
                     : fade.b;
    show(c);
    return c.r == fade.b.r && c.g == fade.b.g && c.b == fade.b.b;
}

/* --- patterns --- */

typedef enum
{
    K_OFF,
    K_READY,
    K_BLINK,
    K_PULSE,
    K_SEQ,
    K_NONE
} kind_t;

/* A sequence of colours with their durations. */
static struct
{
    rgb_t c[12];
    uint16_t ms[12];
    unsigned n, i;
    uint32_t next;
} seq;

static void seq_add(rgb_t c, uint16_t ms)
{
    if (seq.n < 12)
    {
        seq.c[seq.n] = c;
        seq.ms[seq.n++] = ms;
    }
}

/* the 5000 ms one-shots of the extension. */
static void ext_begin(led_request_t r);

static struct
{
    kind_t k;
    rgb_t c;
    int count, done; /* blinks: count 0 = endless */
    bool on, fading;
    uint32_t next;
    uint32_t fade_after; /* one-shots: fade from off to the ready colour */
    int pulse_half;
} pat;

/* Ready display phases, remembered across other patterns. */
typedef enum
{
    PH_BRIGHT,
    PH_FADING,
    PH_RESTING,
    PH_BRIGHTEN
} phase_t;
static phase_t phase = PH_BRIGHT;
static uint32_t bright_end;
static bool bright_next = true;
static rgb_t resume; /* colour reached when the fading phase was left */

static led_request_t current = LED_OFF, remembered = LED_OFF;
static bool oneshot; /* a one-shot pattern or colour change is current */

static bool is_oneshot(led_request_t r)
{
    return r >= LED_CONFIRMATION && r <= LED_EXT_LEVEL_OFF;
}

static bool is_ext(led_request_t r)
{
    return r >= LED_EXT_BLINK && r <= LED_EXT_LEVEL_OFF;
}

static void ready_begin(void)
{
    pat.k = K_READY;
    if (bright_next || phase == PH_BRIGHT || phase == PH_BRIGHTEN)
    {
        bright_next = false;
        phase = PH_BRIGHT;
        bright_end = tick_ms() + 3000;
        show(ready_colour());
    }
    else if (phase == PH_FADING)
    {
        fade_start(resume, ready_target(), 2000);
    }
    else
    {
        show(ready_target());
    }
}

static void blink_begin(rgb_t c, int count, uint32_t fade_after)
{
    pat.k = K_BLINK;
    pat.c = c;
    pat.count = count;
    pat.done = 0;
    pat.on = true;
    pat.fading = false;
    pat.fade_after = fade_after;
    pat.next = tick_ms() + 500;
    show(c);
}

static void begin(led_request_t r)
{
    switch (r)
    {
    case LED_OFF:
        pat.k = K_OFF;
        show(OFF);
        break;
    case LED_READY_IDLE:
    case LED_READY_PLAYING:
        ready_begin();
        break;
    case LED_SYNC:
    case LED_FIRMWARE_DOWNLOAD:
    case LED_CONTENT_DOWNLOAD:
    case LED_POWER_ACTION:
    case LED_CONNECTING:
    case LED_SEARCH:
        blink_begin(BLUE, 0, 0);
        break;
    case LED_SETUP:
        pat.k = K_PULSE;
        pat.pulse_half = 0;
        fade_start(OFF, BLUE, 2000);
        break;
    case LED_NETWORK_ERROR:
        blink_begin(RED, 0, 0);
        break;
    case LED_NEW_IMAGE:
        blink_begin(GREEN, 0, 0);
        break;
    case LED_CONFIRMATION:
        blink_begin(ready_colour(), 3, 4000);
        break;
    case LED_ERROR:
        blink_begin(RED, 30, 2000);
        bright_next = true;
        break;
    case LED_STATION_REMOVED:
        blink_begin(GREEN, 5, 0);
        break;
    case LED_STATION_REMOVED_LOW:
        blink_begin(ORANGE, 5, 0);
        break;
    case LED_BATTERY_EMPTY:
        blink_begin(ORANGE, 15, 0);
        break;
    case LED_BATTERY_LOW:
        blink_begin(ORANGE, 10, 0);
        break;
    case LED_EXT_BLINK:
    case LED_EXT_LILAC:
    case LED_EXT_ON:
    case LED_EXT_OFF:
    case LED_EXT_MIXED:
    case LED_EXT_LEVEL_ON:
    case LED_EXT_LEVEL_DIMMED:
    case LED_EXT_LEVEL_OFF:
        ext_begin(r);
        break;
    default:
        pat.k = K_NONE;
        break;
    }
}

static void leave(void)
{
    if (pat.k == K_READY && phase == PH_BRIGHTEN)
    {
        phase = PH_BRIGHT;
    }
    if (pat.k == K_READY && phase == PH_FADING)
    {
        resume = shown;
    }
}

static void apply(led_request_t r)
{
    if (r == LED_ACTIVITY)
    {
        if (oneshot || (current != LED_READY_IDLE && current != LED_READY_PLAYING) || pat.k != K_READY)
        {
            return;
        }
        if (phase == PH_BRIGHT)
        {
            bright_end = tick_ms() + 3000;
        }
        else
        {
            phase = PH_BRIGHTEN;
            fade_start(shown, ready_colour(), 200);
        }
        return;
    }
    if (r == current && !is_ext(r))
    {
        return; /* an extension display shown again starts anew */
    }
    if (is_oneshot(r) || r == LED_COLOUR_CHANGE)
    {
        if (!oneshot)
        {
            remembered = current;
        }
        leave();
        current = r;
        oneshot = true;
        begin(r);
        return;
    }
    if (oneshot && r == remembered)
    {
        return;
    }
    leave();
    current = r;
    oneshot = false;
    begin(r);
}

static void ext_begin(led_request_t r)
{
    seq.n = seq.i = 0;
    switch (r)
    {
    case LED_EXT_BLINK:
    case LED_EXT_LILAC:
        seq_add(r == LED_EXT_BLINK ? TURQUOISE : LILAC, 500);
        seq_add(OFF, 4500);
        break;
    case LED_EXT_ON:
    case LED_EXT_OFF:
    case LED_EXT_MIXED:
        for (int i = 0; i < 5; i++)
        {
            bool t = r == LED_EXT_ON || (r == LED_EXT_MIXED && !(i & 1));
            seq_add(t ? TURQUOISE : LILAC, 500);
            seq_add(OFF, 500);
        }
        break;
    default:
    {
        /* one blink for on, two for dimmed, three for off, then the level */
        int blinks = r == LED_EXT_LEVEL_ON ? 1 : r == LED_EXT_LEVEL_DIMMED ? 2
                                                                           : 3;
        for (int i = 0; i < blinks; i++)
        {
            seq_add(TURQUOISE, 500);
            seq_add(OFF, 500);
        }
        uint16_t rest = (uint16_t)(5000 - 1000 * blinks);
        if (r == LED_EXT_LEVEL_ON)
        {
            seq_add(TURQUOISE, rest);
        }
        else if (r == LED_EXT_LEVEL_DIMMED)
        {
            seq_add(TURQUOISE, 1000);
            seq_add(TURQUOISE_DIM, (uint16_t)(rest - 1000));
        }
        else
        {
            seq_add(OFF, rest);
        }
        break;
    }
    }
    pat.k = K_SEQ;
    seq.next = tick_ms() + seq.ms[0];
    show(seq.c[0]);
}

static void oneshot_finished(void)
{
    oneshot = false;
    current = remembered;
    begin(current);
}

/* Runs the current pattern; returns the ms to wait. */
static uint32_t run(void)
{
    uint32_t now = tick_ms();
    switch (pat.k)
    {
    case K_OFF:
        return portMAX_DELAY;
    case K_NONE:
        if (oneshot)
        {
            oneshot_finished();
        }
        return 0;
    case K_SEQ:
        if ((int32_t)(now - seq.next) < 0)
        {
            return seq.next - now;
        }
        if (++seq.i >= seq.n)
        {
            oneshot_finished();
            return 0;
        }
        seq.next += seq.ms[seq.i];
        show(seq.c[seq.i]);
        return seq.ms[seq.i];
    case K_PULSE:
        if (fade_step())
        {
            pat.pulse_half ^= 1;
            fade_start(pat.pulse_half ? BLUE : OFF, pat.pulse_half ? OFF : BLUE, 2000);
        }
        return fade.interval;
    case K_BLINK:
        if (pat.fading)
        {
            if (fade_step())
            {
                oneshot_finished();
                return 0;
            }
            return fade.interval;
        }
        if ((int32_t)(now - pat.next) < 0)
        {
            return pat.next - now;
        }
        pat.next += 500;
        if (pat.on)
        {
            show(OFF);
            pat.on = false;
            pat.done++;
            if (pat.count && pat.done >= pat.count)
            {
                if (pat.fade_after)
                {
                    pat.fading = true;
                    fade_start(OFF, ready_colour(), pat.fade_after);
                    return fade.interval;
                }
                if (oneshot)
                {
                    pat.next = now + 500; /* the last off phase */
                    pat.k = K_NONE;
                    return 500;
                }
            }
        }
        else
        {
            show(pat.c);
            pat.on = true;
        }
        return 500;
    case K_READY:
        switch (phase)
        {
        case PH_BRIGHT:
            if ((int32_t)(now - bright_end) < 0)
            {
                return bright_end - now;
            }
            phase = PH_FADING;
            fade_start(ready_colour(), ready_target(), 2000);
            return fade.interval;
        case PH_FADING:
            if (fade_step())
            {
                phase = PH_RESTING;
                return portMAX_DELAY;
            }
            return fade.interval;
        case PH_BRIGHTEN:
            if (fade_step())
            {
                phase = PH_BRIGHT;
                bright_end = now + 3000;
            }
            return fade.interval;
        case PH_RESTING:
            show(ready_target());
            return portMAX_DELAY;
        }
    }
    return portMAX_DELAY;
}

static StaticQueue_t queue_mem;
static uint8_t queue_buf[8];
static QueueHandle_t queue;

void ledui_request(led_request_t r)
{
    uint8_t v = (uint8_t)r;
    if (queue)
    {
        xQueueSend(queue, &v, 0);
    }
}

static void task(void *arg)
{
    (void)arg;
    ready_begin(); /* lit from the start, not dark until the NWP runs */
    for (;;)
    {
        uint32_t wait = run();
        uint8_t r;
        if (xQueueReceive(queue, &r, wait) == pdTRUE)
        {
            apply((led_request_t)r);
        }
    }
}

void ledui_start(void)
{
    static StackType_t stack[TASK_STACK(288)]; /* worst case 616 B, +25 % */
    static StaticTask_t tcb;
    queue = xQueueCreateStatic(8, 1, queue_buf, &queue_mem);
    xTaskCreateStatic(task, "led", TASK_STACK(288), NULL, 4, stack, &tcb); /* above the player, as the sound activity */
}
