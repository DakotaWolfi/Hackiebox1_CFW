/*
 * The player's part of the extension: settings in effect, event tags and their
 * confirmation, values set with the
 * ears, the locked gesture record and the sleep timer.
 *
 * Files are read when needed (profile and event tag file at a placement,
 * the global file at the start and after a settings file was stored); only
 * the decoded values are kept.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "debug.h"
#include "ext.h"
#include "ledui.h"
#include "player.h"
#include "power.h"
#include "rtnl.h"
#include "settings.h"
#include "sound.h"
#include "tick.h"
#include "xplay.h"

/* --- state --- */

static ext_settings_t glob; /* empty when absent */
static uint32_t glob_generation;

static bool fig_on; /* a figure (not an event tag) stands */
static uint8_t fig_uid[8];
static ext_settings_t fig; /* its profile, empty when absent */

/* The event tag that stands. */
static struct
{
    bool on;
    uint8_t uid[8];
    ext_event_t e;
    ext_scope_t scope;
    int vol, led;   /* index of the volume / LED action using the ears, -1 */
    uint8_t chosen; /* LED level chosen (0 on, 1 off, 2 dimmed) */
    bool led_phase; /* combined tag: the ears set the LED level */
} ev;

/* Actions kept for the next figure. */
static struct
{
    bool on;
    ext_event_t e;
    uint32_t removed_ms;
    bool removed;
    uint8_t vol_path, vol_step, led;
} kept;

/* Read by the input activity (player.h). */
static volatile uint8_t ears_mode = EARS_NORMAL;
static volatile bool ears_combined;
static volatile int8_t lock_value[3] = {-1, -1, -1};
static volatile uint8_t lock_source[3];
static volatile bool ears_off = true; /* F9 allowed */

player_ears_t player_ears(void)
{
    return (player_ears_t)ears_mode;
}

bool player_ears_combined(void)
{
    return ears_combined;
}

int player_lock(player_lock_t l, unsigned *source)
{
    if (source)
    {
        *source = lock_source[l];
    }
    return lock_value[l];
}

/* --- settings in effect --- */

static bool has(const ext_settings_t *s, ext_key_t k)
{
    return ext_has(s, k);
}

/* The figure's value, else the global file's, else -1. */
static int effect(ext_key_t k, bool with_figure)
{
    if (with_figure && fig_on && has(&fig, k))
    {
        return fig.value[k];
    }
    if (has(&glob, k))
    {
        return glob.value[k];
    }
    return -1;
}

/* LED level in effect without the event tag's chosen one; for start value
 * 3 the level in effect, the server's when none is set. */
static int led_in_effect(void);

static void apply(void)
{
    int ls, lh, led;
    if (ev.on && ev.vol >= 0)
    {
        ls = lh = 15; /* lifted while the tag stands */
    }
    else
    {
        ls = effect(EXT_SPEAKER_LIMIT, true);
        lh = effect(EXT_HEADPHONE_LIMIT, true);
    }
    sound_ext_limits(ls < 0 ? 0xFF : (uint8_t)ls, lh < 0 ? 0xFF : (uint8_t)lh);
    led = ev.on && ev.led >= 0 ? ev.chosen : effect(EXT_LED, true);
    ledui_ext_level(led);
    static const ext_key_t lk[3] = {EXT_SLAP_LOCK, EXT_TILT_LOCK, EXT_VOLUME_LOCK};
    for (int i = 0; i < 3; i++)
    {
        if (fig_on && has(&fig, lk[i]))
        {
            lock_value[i] = (int8_t)fig.value[lk[i]];
            lock_source[i] = 1;
        }
        else if (has(&glob, lk[i]))
        {
            lock_value[i] = (int8_t)glob.value[lk[i]];
            lock_source[i] = 2;
        }
        else
        {
            lock_value[i] = -1;
            lock_source[i] = 0;
        }
    }
    ears_off = !has(&glob, EXT_BOTH_EARS) || glob.value[EXT_BOTH_EARS]; /* absent = on */
    power_idle_minutes(glob.value[EXT_IDLE_BATTERY], glob.value[EXT_IDLE_STATION]);
    ears_combined = ev.on && ev.vol >= 0 && ev.led >= 0;
    if (!ev.on || (ev.vol < 0 && ev.led < 0))
    {
        ears_mode = EARS_NORMAL;
    }
    else if (ev.led >= 0 && (ev.vol < 0 || ev.led_phase))
    {
        ears_mode = EARS_LED;
    }
    else
    {
        ears_mode = EARS_VOLUME;
    }
}

static int led_in_effect(void)
{
    int l = effect(EXT_LED, true);
    return l >= 0 ? l : settings.led;
}

static void load_global(void)
{
    glob_generation = ext_generation();
    if (!ext_global(&glob))
    {
        memset(&glob, 0, sizeof glob);
    }
}

void xp_start(void)
{
    load_global();
    apply();
}

/* --- confirmation --- */

static void confirm(uint16_t results, unsigned n, bool tones, bool led)
{
    if (!n)
    {
        return;
    }
    uint16_t all = (uint16_t)((1u << n) - 1);
    debug_verbose("ext: confirm %u, %04x\n", n, results);
    if (led)
    {
        ledui_request((results & all) == all ? LED_EXT_ON : (results & all) == 0 ? LED_EXT_OFF
                                                                                 : LED_EXT_MIXED);
    }
    if (tones)
    {
        sound_feedback(results, n);
    }
}

/* --- sleep timer --- */

static struct
{
    bool on, paused, once, fading;
    uint8_t uid[8];
    uint32_t total, elapsed; /* s */
    uint32_t fade_time, v;   /* fade time F and volume step v */
    uint8_t base[3], reached, path;
    uint32_t ears, next_ms;
    int last_s;
} tm;

static void timer_restore(void)
{
    for (unsigned p = 1; p <= 2; p++)
    {
        sound_set_step(p, tm.base[p]);
    }
}

static void timer_start(const uint8_t uid[8], uint32_t minutes, bool once)
{
    memset(&tm, 0, sizeof tm);
    tm.on = true;
    tm.once = once;
    memcpy(tm.uid, uid, 8);
    tm.total = 60 * minutes;
    tm.base[1] = (uint8_t)sound_step(1);
    tm.base[2] = (uint8_t)sound_step(2);
    tm.path = (uint8_t)sound_path();
    tm.next_ms = tick_ms() + 1000;
    tm.last_s = -1;
    debug_verbose("ext: sleep timer %lu min%s\n", (unsigned long)minutes, once ? " (once)" : "");
}

static void timer_end(void)
{
    if (tm.on && !tm.paused)
    {
        timer_restore();
    }
    tm.on = false;
}

/* One second while the figure stands; true when the time is up. */
static bool timer_second(void)
{
    tm.elapsed++;
    unsigned path = sound_path();
    uint32_t r = tm.total - tm.elapsed;
    if (path != tm.path)
    {
        /* the old output's step back, the new one from its
         * current step */
        if (tm.path == 1 || tm.path == 2)
        {
            sound_set_step(tm.path, tm.base[tm.path]);
        }
        tm.path = (uint8_t)path;
        if (tm.fading)
        {
            tm.v = sound_step(path);
            tm.fade_time = r;
            tm.last_s = -1;
        }
    }
    if (r == 0)
    {
        debug_verbose("ext: sleep timer up\n");
        timer_restore();
        tm.on = false;
        return true;
    }
    if (!tm.fading && tm.elapsed >= tm.total / 3)
    {
        tm.fading = true;
        tm.v = sound_step(path);
        tm.fade_time = r;
        tm.ears = sound_ear_steps();
        tm.last_s = -1;
    }
    if (!tm.fading)
    {
        return false;
    }
    if (sound_ear_steps() != tm.ears)
    {
        /* an ear step during the fade: from the new step over the time
         * left */
        tm.ears = sound_ear_steps();
        tm.v = sound_step(path);
        tm.fade_time = r;
    }
    int s = tm.fade_time ? (int)(16 * tm.v * r / tm.fade_time) : 0;
    if (s != tm.last_s)
    {
        sound_fade((unsigned)s);
        tm.last_s = s;
    }
    return false;
}

/* paused as on removal, the step from before the timer back;
 * continued from the step the fade had reached. */
static void timer_pause(void)
{
    tm.paused = true;
    tm.reached = (uint8_t)sound_step(tm.path);
    timer_restore();
}

static void timer_continue(void)
{
    tm.paused = false;
    tm.path = (uint8_t)sound_path();
    sound_set_step(0, tm.reached);
    tm.ears = sound_ear_steps();
    tm.next_ms = tick_ms() + 1000;
    tm.last_s = -1;
    debug_verbose("ext: sleep timer continues, %lu s left\n", (unsigned long)(tm.total - tm.elapsed));
}

void xp_timer_hold(bool hold)
{
    if (!tm.on || !fig_on || hold == tm.paused)
    {
        return;
    }
    if (hold)
    {
        timer_pause();
    }
    else
    {
        timer_continue();
    }
}

/* a power-off keeps the steps from before the timer; read by
 * the power activity (power.h). */
void player_timer_steps(uint8_t step[2])
{
    if (tm.on && !tm.paused)
    {
        step[0] = tm.base[1];
        step[1] = tm.base[2];
    }
}

/* --- polling --- */

bool xp_poll(void)
{
    if (ext_generation() != glob_generation)
    {
        /* a stored file: the global values, and the profile of the figure
         * on the box, take effect at once */
        load_global();
        if (fig_on && !ext_profile(fig_uid, &fig))
        {
            memset(&fig, 0, sizeof fig);
        }
        apply();
    }
    if (!tm.on || tm.paused || !fig_on)
    {
        return false;
    }
    bool up = false;
    while (!up && tm.on && (int32_t)(tick_ms() - tm.next_ms) >= 0)
    {
        tm.next_ms += 1000;
        up = timer_second();
    }
    return up;
}

/* --- event tags --- */

static bool led_action(uint8_t a, ext_scope_t sc)
{
    return sc == EXT_SCOPE_FIGURE ? a == 6 : a == 8;
}

static bool vol_action(uint8_t a, ext_scope_t sc)
{
    return sc == EXT_SCOPE_FIGURE ? a == 5 : a == 9;
}

static bool in_scope(uint8_t a, ext_scope_t sc)
{
    if (sc == EXT_SCOPE_FIGURE)
    {
        return a >= 1 && a <= 12 && a != 8 && a != 9;
    }
    return a == 8 || a == 9 || (a >= 13 && a <= 18);
}

/* The record set to 1 (true) or removed by mode: toggle by the value t
 * in effect; on result. */
static bool set_lock(ext_settings_t *s, ext_key_t k, uint8_t mode, bool in_effect)
{
    bool on;
    switch (mode)
    {
    case EXT_TOGGLE:
        on = !in_effect;
        break;
    case EXT_ON:
        on = true;
        break;
    case EXT_OFF:
        on = false;
        break;
    default:
        ext_clear(s, k);
        return false;
    }
    ext_set(s, k, on);
    return on;
}

/* actions 13 to 18 at the placement. */
static void global_actions(void)
{
    ext_settings_t g;
    if (!ext_global(&g))
    {
        memset(&g, 0, sizeof g);
    }
    uint16_t results = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < ev.e.n; i++)
    {
        uint8_t a = ev.e.a[i].action, mode = ev.e.a[i].mode;
        if (a < 13 || a > 18)
        {
            continue;
        }
        bool on;
        if (a >= 17)
        {
            ext_key_t k = a == 17 ? EXT_IDLE_BATTERY : EXT_IDLE_STATION;
            uint16_t minutes = ev.e.a[i].value;
            on = minutes != 0;
            if (on)
            {
                ext_set(&g, k, minutes);
            }
            else
            {
                ext_clear(&g, k);
            }
        }
        else if (a == 16)
        {
            bool cur = !ext_has(&g, EXT_BOTH_EARS) || g.value[EXT_BOTH_EARS];
            on = set_lock(&g, EXT_BOTH_EARS, mode, cur);
        }
        else
        {
            ext_key_t k = (ext_key_t)(EXT_SLAP_LOCK + (a - 13));
            /* the box's lock in effect without a figure */
            bool cur = ext_has(&g, k) && g.value[k];
            on = set_lock(&g, k, mode, cur);
        }
        if (on)
        {
            results |= (uint16_t)(1u << n);
        }
        n++;
    }
    if (!n)
    {
        return;
    }
    ext_write(NULL, &g);
    glob = g;
    glob_generation = ext_generation();
    apply();
    /* the level display wins on the LED */
    confirm(results, n, ev.e.tones, !(ev.led >= 0 && ev.vol < 0));
}

static void level_display(void)
{
    static const led_request_t r[3] = {LED_EXT_LEVEL_ON, LED_EXT_LEVEL_OFF, LED_EXT_LEVEL_DIMMED};
    ledui_request(r[ev.chosen <= 2 ? ev.chosen : 0]);
}

static bool place_event(const uint8_t uid[8])
{
    ext_event_t e;
    if (!ext_event_tag(uid, &e))
    {
        return false;
    }
    memset(&ev, 0, sizeof ev);
    ev.on = true;
    memcpy(ev.uid, uid, 8);
    ev.e = e;
    ev.scope = ext_event_scope(&e);
    ev.vol = ev.led = -1;
    kept.on = false; /* replaced */
    /* the first volume and the first LED action count; in
     * mode clear they do not use the ears */
    bool seen_vol = false, seen_led = false;
    for (unsigned i = 0; i < e.n; i++)
    {
        uint8_t a = e.a[i].action;
        if (!in_scope(a, ev.scope))
        {
            continue;
        }
        if (vol_action(a, ev.scope) && !seen_vol)
        {
            seen_vol = true;
            if (e.a[i].mode != EXT_CLEAR)
            {
                ev.vol = (int)i;
            }
        }
        if (led_action(a, ev.scope) && !seen_led)
        {
            seen_led = true;
            if (e.a[i].mode != EXT_CLEAR)
            {
                ev.led = (int)i;
            }
        }
    }
    debug_printf("ext: event tag %u/%d/%d/%d\n", e.n, ev.scope, ev.vol, ev.led);
    if (ev.led >= 0)
    {
        unsigned v = e.a[ev.led].value;
        ev.chosen = (uint8_t)(v <= 2 ? (int)v : led_in_effect());
    }
    bool globals = false;
    if (ev.scope == EXT_SCOPE_GLOBAL)
    {
        for (unsigned i = 0; i < e.n; i++)
        {
            globals |= e.a[i].action >= 13 && e.a[i].action <= 18;
        }
    }
    apply();
    if (ev.vol >= 0)
    {
        unsigned p = e.a[ev.vol].value;
        if (p >= 1 && p <= 16)
        {
            sound_set_step(0, p - 1);
        }
    }
    /* the LED on placing */
    if (ev.led >= 0 && ev.vol < 0)
    {
        level_display();
    }
    else if (!globals)
    {
        ledui_request(LED_EXT_BLINK);
    }
    if (globals)
    {
        global_actions();
    }
    return true;
}

/* the removal of an event tag. */
static void remove_event(void)
{
    unsigned path = sound_path();
    uint8_t step = (uint8_t)sound_step(path);
    if (ev.scope == EXT_SCOPE_FIGURE)
    {
        kept.on = true;
        kept.e = ev.e;
        kept.removed = true;
        kept.removed_ms = tick_ms();
        kept.vol_path = (uint8_t)path;
        kept.vol_step = step;
        kept.led = ev.chosen;
    }
    else if (ev.scope == EXT_SCOPE_GLOBAL)
    {
        /* the first action 9 and the first action 8, clear included */
        ext_settings_t g;
        if (!ext_global(&g))
        {
            memset(&g, 0, sizeof g);
        }
        uint16_t results = 0;
        unsigned n = 0;
        bool seen_vol = false, seen_led = false;
        for (unsigned i = 0; i < ev.e.n; i++)
        {
            uint8_t a = ev.e.a[i].action, mode = ev.e.a[i].mode;
            bool on = false;
            if (a == 9 && !seen_vol)
            {
                seen_vol = true;
                if (mode == EXT_CLEAR)
                {
                    ext_clear(&g, EXT_SPEAKER_LIMIT);
                    ext_clear(&g, EXT_HEADPHONE_LIMIT);
                }
                else if (path == 1 || path == 2)
                {
                    ext_set(&g, path == 1 ? EXT_SPEAKER_LIMIT : EXT_HEADPHONE_LIMIT, step);
                    on = true;
                }
            }
            else if (a == 8 && !seen_led)
            {
                seen_led = true;
                if (mode == EXT_CLEAR)
                {
                    ext_clear(&g, EXT_LED);
                }
                else
                {
                    ext_set(&g, EXT_LED, ev.chosen);
                    on = true;
                }
            }
            else
            {
                continue;
            }
            if (on)
            {
                results |= (uint16_t)(1u << n);
            }
            n++;
        }
        if (n)
        {
            ext_write(NULL, &g);
            glob = g;
            glob_generation = ext_generation();
            confirm(results, n, ev.e.tones, true);
        }
    }
    ev.on = false;
    apply();
}

/* --- kept actions for the next figure --- */

static void apply_kept(const uint8_t uid[8], ext_settings_t *p, uint32_t *once_minutes, bool *once)
{
    uint16_t results = 0;
    unsigned n = 0;
    bool seen_vol = false, seen_led = false;
    for (unsigned i = 0; i < kept.e.n; i++)
    {
        uint8_t a = kept.e.a[i].action, mode = kept.e.a[i].mode;
        uint16_t val = kept.e.a[i].value;
        if (!in_scope(a, EXT_SCOPE_FIGURE))
        {
            continue;
        }
        bool on = false;
        switch (a)
        {
        case 1:
        case 2:
        {
            ext_key_t k = a == 1 ? EXT_RESUME : EXT_SHUFFLE;
            if (mode == EXT_ON || (mode == EXT_TOGGLE && !(ext_has(p, k) && p->value[k] == 1)))
            {
                ext_set(p, k, 1);
                on = true;
            }
            else
            {
                ext_clear(p, k);
            }
            break;
        }
        case 3:
            if (val && (mode == EXT_ON ||
                        (mode == EXT_TOGGLE && !(ext_has(p, EXT_SLEEP) && p->value[EXT_SLEEP] == val))))
            {
                ext_set(p, EXT_SLEEP, val);
                on = true;
            }
            else
            {
                ext_clear(p, EXT_SLEEP);
            }
            break;
        case 4:
            *once = true;
            *once_minutes = val;
            on = val != 0;
            break;
        case 5:
            if (seen_vol)
            {
                continue;
            }
            seen_vol = true;
            if (mode == EXT_CLEAR)
            {
                ext_clear(p, EXT_SPEAKER_LIMIT);
                ext_clear(p, EXT_HEADPHONE_LIMIT);
            }
            else if (kept.vol_path == 1 || kept.vol_path == 2)
            {
                ext_set(p, kept.vol_path == 1 ? EXT_SPEAKER_LIMIT : EXT_HEADPHONE_LIMIT, kept.vol_step);
                on = true;
            }
            break;
        case 6:
            if (seen_led)
            {
                continue;
            }
            seen_led = true;
            if (mode == EXT_CLEAR)
            {
                ext_clear(p, EXT_LED);
            }
            else
            {
                ext_set(p, EXT_LED, kept.led);
                on = true;
            }
            break;
        case 7:
            p->present = 0;
            memset(p->value, 0, sizeof p->value);
            break;
        default:
        { /* 10 to 12: the lock in effect for this figure */
            ext_key_t k = (ext_key_t)(EXT_SLAP_LOCK + (a - 10));
            bool cur = ext_has(p, k) ? p->value[k] != 0 : ext_has(&glob, k) && glob.value[k];
            on = set_lock(p, k, mode, cur);
            break;
        }
        }
        if (on)
        {
            results |= (uint16_t)(1u << n);
        }
        n++;
    }
    ext_write(uid, p); /* also when nothing changed */
    confirm(results, n, kept.e.tones, true);
}

/* --- placement and removal --- */

bool xp_place(const uint8_t uid[8])
{
    if (ev.on || fig_on)
    {
        xp_remove(); /* a removal not acted on counts as one now */
    }
    if (place_event(uid))
    {
        return true;
    }
    fig_on = true;
    memcpy(fig_uid, uid, 8);
    if (!ext_profile(uid, &fig))
    {
        memset(&fig, 0, sizeof fig);
    }
    uint32_t once_minutes = 0;
    bool once = false;
    if (kept.on && kept.removed && tick_ms() - kept.removed_ms <= 60000)
    {
        debug_verbose("ext: kept actions\n");
        apply_kept(uid, &fig, &once_minutes, &once);
    }
    kept.on = false;
    once = once && once_minutes; /* N = 0: no one-time timer */
    apply();
    /* the one-time timer first, then a paused
     * timer of the same figure, else the figure's own */
    if (tm.on && (once || memcmp(tm.uid, uid, 8)))
    {
        timer_end();
    }
    if (once)
    {
        timer_start(uid, once_minutes, true);
    }
    else if (tm.on && tm.paused)
    {
        timer_continue();
    }
    else if (!tm.on && ext_has(&fig, EXT_SLEEP) && fig.value[EXT_SLEEP])
    {
        timer_start(uid, fig.value[EXT_SLEEP], false);
    }
    return false;
}

const ext_settings_t *xp_profile(void)
{
    return &fig;
}

void xp_remove(void)
{
    if (ev.on)
    {
        remove_event();
        return;
    }
    if (!fig_on)
    {
        return;
    }
    if (tm.on && !tm.paused)
    {
        timer_pause();
    }
    fig_on = false;
    apply();
}

/* --- ears and locks --- */

void xp_ear_level(bool big)
{
    (void)big;
    if (!ev.on || ears_mode != EARS_LED)
    {
        return;
    }
    /* the ring on -> dimmed -> off -> on */
    ev.chosen = ev.chosen == 0 ? 2 : ev.chosen == 2 ? 1
                                                    : 0;
    apply();
    level_display();
}

void xp_ears_switch(void)
{
    if (!ev.on || !ears_combined)
    {
        return;
    }
    ev.led_phase = !ev.led_phase;
    apply();
    if (ev.led_phase)
    {
        level_display();
    }
    else
    {
        ledui_request(LED_EXT_BLINK);
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void xp_locked(unsigned gesture, unsigned source)
{
    debug_printf("ext: gesture %u locked, source %u\n", gesture, source);
    if (!ext_hello_received())
    {
        return;
    }
    static const uint8_t none[8];
    const uint8_t *u = fig_on ? fig_uid : ev.on ? ev.uid
                                                : none;
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)gesture);
    log_arg_num(&a, (int32_t)source);
    log_arg_num(&a, (int32_t)le32(u));
    log_arg_num(&a, (int32_t)le32(u + 4));
    log_fn(LOG_INFO, 22, 32259, 0, &a);
}

bool xp_ears_off(void)
{
    return ears_off;
}

void xp_power_off(void)
{
    xp_remove();
    tm.on = false; /* every timer ends */
}
