/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "port.h"
#include "audio_out.h"
#include "bootstate.h"
#include "codec.h"
#include "power.h"
#include "debug.h"
#include "sound.h"
#include "ledui.h"
#include "rtnl.h"
#include "settings.h"
#include "tick.h"
#include "tones.h"

typedef enum
{
    REQ_EAR_BIG,
    REQ_EAR_SMALL,
    REQ_CHIME,
    REQ_WAKE, /* only wakes the activity for the state below */
    REQ_LIMITS,
    REQ_SET_STEP, /* arg: path << 4 | step, path 0 the current one */
    REQ_FADE,     /* arg: step position in sixteenths */
    REQ_FEEDBACK, /* the tone pairs in fb */
} request_kind_t;

typedef struct
{
    uint8_t kind;
    uint8_t arg;
} request_t;

static const int8_t curve_speaker[16] = {-40, -37, -34, -31, -28, -25, -22, -18,
                                         -15, -12, -9, -6, -3, 0, 3, 5};
static const int8_t curve_headphones[16] = {-40, -36, -32, -28, -26, -24, -22, -20,
                                            -18, -16, -14, -12, -10, -8, -6, -4};

typedef struct
{
    uint8_t step;
    uint8_t limit;
} path_volume_t;

static struct
{
    bool configured;
    codec_path_t path;
    bool dac_unmuted;
    bool headset; /* last reported state */
    path_volume_t vol[3];
} st;

/* End of the last path switch for a headphone change, 0 before any. */
static volatile uint32_t headphone_ref;

/* Wanted state, written by other activities and the audio interrupt and
 * applied by the sound activity after each request and during fades; the
 * request posted with a change only wakes the activity,
 * so a full queue loses no mute, unmute or headset read. */
static volatile bool want_unmuted, unmute_waiting, headset_wanted, park_wanted;
static bool parked; /* no further handling */

/* Limits of the extension per path,
 * 0xFF: the server limit. */
static volatile uint8_t ext_limit[3] = {0xFF, 0xFF, 0xFF};
/* Read by the player: steps, the selected path, ear steps made. */
static volatile uint8_t pub_step[3] = {0, 8, 8}, pub_limit[3] = {0, 15, 15}, pub_path;
static volatile uint32_t ear_steps;
static volatile uint16_t fb_results;
static volatile uint8_t fb_count;
static bool preset;
static uint8_t preset_step[2];
static StaticSemaphore_t unmuted_mem, headset_mem, parked_mem;
static SemaphoreHandle_t unmuted_sem, headset_sem, parked_sem;

#define QUEUE_LEN 8
static StaticQueue_t queue_mem;
static uint8_t queue_buf[QUEUE_LEN * sizeof(request_t)];
static QueueHandle_t queue;

#define STACK TASK_STACK(376) /* worst case 1192 B (make stackcheck), +25 % */
static StackType_t stack[STACK];
static StaticTask_t tcb;

static int gain_of(codec_path_t p, int step)
{
    return p == PATH_HEADPHONES ? curve_headphones[step] : curve_speaker[step];
}

static int gain_current(int step)
{
    return gain_of(st.path, step);
}

/* record 8672, the volume as set. */
static void log_volume(int step)
{
    log_args_t a = {0};
    log_arg_num(&a, st.path == PATH_HEADPHONES ? 8671 : st.path == PATH_SPEAKER ? 8670
                                                                                : 8669);
    log_arg_num(&a, gain_of(st.path, step));
    log_arg_num(&a, step);
    log_fn(LOG_INFO, 27, 8672, 0, &a);
}

static void apply_volume(void)
{
    if (st.path == PATH_PARKED)
    {
        return;
    }
    path_volume_t *v = &st.vol[st.path];
    if (v->step > v->limit)
    {
        v->step = v->limit;
    }
    codec_set_gain_db(gain_of(st.path, v->step));
    log_volume(v->step);
}

/* -63 dB, register 64, the gain of the step again; each
 * reported. */
static void dac_set(bool unmuted)
{
    codec_set_gain_db(-63);
    codec_dac_mute(!unmuted);
    st.dac_unmuted = unmuted;
    apply_volume();
    debug_verbose("DAC %s\n", unmuted ? "unmuted" : "muted");
}

static void select_path(codec_path_t p)
{
    if (p == st.path)
    {
        return;
    }
    bool was_unmuted = st.dac_unmuted;
    if (was_unmuted)
    {
        dac_set(false);
    }
    codec_path_writes(p);
    st.path = p;
    apply_volume();
    if (was_unmuted && p != PATH_PARKED)
    {
        dac_set(true);
    }
    debug_printf("snd: path %s\n", p == PATH_SPEAKER ? "speaker" : p == PATH_HEADPHONES ? "headphones"
                                                                                        : "parked");
}

/* after a bus recovery the codec is configured from scratch
 * and the path selected from the headset state read at its end; the
 * remembered headset state and the volume steps are kept. */
static void after_recovery(void)
{
    bool headset, unmuted = st.dac_unmuted;
    if (!codec_configure(&headset))
    {
        debug_printf("snd: codec after recovery failed\n");
        st.configured = false;
        return;
    }
    st.path = PATH_PARKED;
    st.dac_unmuted = false;
    select_path(headset ? PATH_HEADPHONES : PATH_SPEAKER);
    if (unmuted)
    {
        dac_set(true);
    }
    debug_printf("snd: codec after recovery ok\n");
}

/* the DAC follows the wanted stream state; a waiting
 * sound_stream_start returns once this has run. */
static void reconcile_stream(void)
{
    bool waiting = unmute_waiting; /* set after want_unmuted */
    bool want = want_unmuted;
    if (st.configured && !parked && st.path != PATH_PARKED && want != st.dac_unmuted)
    {
        dac_set(want);
    }
    if (waiting)
    {
        unmute_waiting = false;
        xSemaphoreGive(unmuted_sem);
    }
}

/* step position s in sixteenths of a step; the step becomes
 * floor(s / 16), the gain lies between it and the next in 0.5 dB units. */
static void fade_to(unsigned s)
{
    if (!st.configured || st.path == PATH_PARKED)
    {
        return;
    }
    unsigned k = s / 16;
    if (k > 15)
    {
        k = 15, s = 240;
    }
    path_volume_t *v = &st.vol[st.path];
    v->step = (uint8_t)k;
    int g = gain_of(st.path, (int)k), half = 2 * g;
    if (k < 15)
    {
        half += 2 * (gain_of(st.path, (int)k + 1) - g) * (int)(s % 16) / 16;
    }
    codec_write(CODEC_REG(0, 65), (uint8_t)(int8_t)half);
    codec_write(CODEC_REG(0, 66), (uint8_t)(int8_t)half);
}

static void publish(void)
{
    pub_step[PATH_SPEAKER] = st.vol[PATH_SPEAKER].step;
    pub_step[PATH_HEADPHONES] = st.vol[PATH_HEADPHONES].step;
    pub_limit[PATH_SPEAKER] = st.vol[PATH_SPEAKER].limit;
    pub_limit[PATH_HEADPHONES] = st.vol[PATH_HEADPHONES].limit;
    pub_path = (uint8_t)st.path;
}

static void ear(bool big)
{
    if (!st.configured || st.path == PATH_PARKED)
    {
        return;
    }
    path_volume_t *v = &st.vol[st.path];
    if (v->step > v->limit)
    {
        v->step = v->limit;
    }
    chime_t c;
    if (big)
    {
        c = v->step < v->limit ? CHIME_VOLUME_UP : CHIME_VOLUME_LIMIT;
        if (v->step < v->limit)
        {
            v->step++;
        }
    }
    else
    {
        c = v->step > 0 ? CHIME_VOLUME_DOWN : CHIME_VOLUME_MIN;
        if (v->step > 0)
        {
            v->step--;
        }
    }
    codec_set_gain_db(gain_of(st.path, v->step));
    log_volume(v->step);
    ear_steps++;
    publish();
    debug_printf("snd: step %u (%d dB)\n", v->step, gain_of(st.path, v->step));
    tones_play_chime(c, v->step, gain_current, reconcile_stream);
}

static void check_headset(void)
{
    bool now;

    if (!st.configured || parked || !codec_headset_inserted(&now) || now == st.headset)
    {
        return;
    }
    st.headset = now;
    select_path(now ? PATH_HEADPHONES : PATH_SPEAKER);
    headphone_ref = tick_ms();
    ledui_request(LED_ACTIVITY);
    log_event(now ? EVT_HEADPHONES_IN : EVT_HEADPHONES_OUT, NULL);
    debug_printf("snd: headphones %s\n", now ? "in" : "out");
}

static void task(void *arg)
{
    (void)arg;
    bool headset = false;

    /* Start values, or the steps kept across the off state
     * (sound_volume_set before the start). CFW_START_STEP is
     * 8 except in development builds. */
    st.vol[PATH_SPEAKER] = (path_volume_t){preset ? preset_step[0] : CFW_START_STEP, 15};
    st.vol[PATH_HEADPHONES] = (path_volume_t){preset ? preset_step[1] : CFW_START_STEP, 15};
    st.path = PATH_PARKED;

    bool configured = codec_configure(&headset);
    bootstate_report(BOOT_CHECK_CODEC, configured); /* trial-start confirmation */
    if (!configured)
    {
        debug_printf("snd: codec config failed\n");
    }
    else
    {
        codec_lost();
        audio_out_start();
        st.configured = true;
        /* The read at the end of the configuration selects the first path
         * but does not change the reported state. */
        select_path(headset ? PATH_HEADPHONES : PATH_SPEAKER);
    }
    publish();

    for (;;)
    {
        request_t r;
        xQueueReceive(queue, &r, portMAX_DELAY);
        if (parked)
        {
            r.kind = REQ_WAKE;
        }
        switch (r.kind)
        {
        case REQ_EAR_BIG:
            ear(true);
            break;
        case REQ_EAR_SMALL:
            ear(false);
            break;
        case REQ_CHIME:
            debug_printf("snd: chime %u\n", r.arg);
            if (st.configured && st.path != PATH_PARKED)
            {
                uint32_t late = tones_play_chime((chime_t)r.arg, st.vol[st.path].step, gain_current, reconcile_stream);
                if (late > 15)
                {
                    debug_printf("snd: chime %u late %lu ms\n", r.arg, (unsigned long)late);
                }
            }
            break;
        }
        if (r.kind == REQ_LIMITS && !parked)
        {
            /* a step above a new limit is lowered at once,
             * without a chime; raising a limit raises no step. The
             * extension's limit replaces the server limit. */
            uint8_t ls = ext_limit[PATH_SPEAKER], lh = ext_limit[PATH_HEADPHONES];
            st.vol[PATH_SPEAKER].limit = ls <= 15 ? ls : settings_limit_step(settings.limit_speaker);
            st.vol[PATH_HEADPHONES].limit = lh <= 15 ? lh : settings_limit_step(settings.limit_headphones);
            if (st.configured)
            {
                apply_volume();
            }
        }
        if (r.kind == REQ_SET_STEP && !parked)
        {
            codec_path_t p = (r.arg >> 4) ? (codec_path_t)(r.arg >> 4) : st.path;
            if (p == PATH_SPEAKER || p == PATH_HEADPHONES)
            {
                st.vol[p].step = r.arg & 15;
                if (st.configured && p == st.path)
                {
                    apply_volume(); /* lowered to the limit */
                }
                else if (st.vol[p].step > st.vol[p].limit)
                {
                    st.vol[p].step = st.vol[p].limit;
                }
            }
        }
        if (r.kind == REQ_FADE && !parked)
        {
            fade_to(r.arg);
        }
        if (r.kind == REQ_FEEDBACK && !parked && st.configured && st.path != PATH_PARKED)
        {
            tones_play_feedback(fb_results, fb_count, st.vol[st.path].step, gain_current, reconcile_stream);
        }
        if (st.configured && !parked && codec_lost())
        {
            after_recovery();
        }
        reconcile_stream();
        publish();
        if (headset_wanted)
        {
            headset_wanted = false;
            check_headset();
            xSemaphoreGive(headset_sem);
        }
        if (park_wanted && !parked)
        {
            /* mute, parked path, then nothing more. */
            if (st.configured)
            {
                if (st.dac_unmuted)
                {
                    dac_set(false);
                }
                select_path(PATH_PARKED);
            }
            parked = true;
        }
        if (park_wanted)
        {
            xSemaphoreGive(parked_sem);
        }
    }
}

static void post(uint8_t kind, uint8_t arg)
{
    request_t r = {kind, arg};
    xQueueSend(queue, &r, 0);
}

void sound_init(void)
{
    queue = xQueueCreateStatic(QUEUE_LEN, sizeof(request_t), queue_buf, &queue_mem);
    unmuted_sem = xSemaphoreCreateBinaryStatic(&unmuted_mem);
    headset_sem = xSemaphoreCreateBinaryStatic(&headset_mem);
    parked_sem = xSemaphoreCreateBinaryStatic(&parked_mem);
}

void sound_start(void)
{
    /* above the player (3): with time slicing off, a decode burst after a
     * jump would otherwise hold back the tone timing for hundreds of ms */
    xTaskCreateStatic(task, "sound", STACK, NULL, 4, stack, &tcb);
}

void sound_ear_big(void)
{
    post(REQ_EAR_BIG, 0);
}

void sound_ear_small(void)
{
    post(REQ_EAR_SMALL, 0);
}

void sound_chime(chime_t c)
{
    post(REQ_CHIME, (uint8_t)c);
}

void sound_check_headset(void)
{
    headset_wanted = true;
    post(REQ_WAKE, 0);
}

bool sound_check_headset_sync(uint32_t max_ms)
{
    xSemaphoreTake(headset_sem, 0);
    headset_wanted = true;
    post(REQ_WAKE, 0);
    return xSemaphoreTake(headset_sem, pdMS_TO_TICKS(max_ms)) == pdTRUE;
}

void sound_stream(bool playing)
{
    want_unmuted = playing;
    post(REQ_WAKE, 0);
}

bool sound_stream_start(uint32_t max_ms)
{
    xSemaphoreTake(unmuted_sem, 0);
    want_unmuted = true;
    unmute_waiting = true;
    post(REQ_WAKE, 0);
    return xSemaphoreTake(unmuted_sem, pdMS_TO_TICKS(max_ms)) == pdTRUE;
}

void sound_limits_changed(void)
{
    post(REQ_LIMITS, 0);
}

void sound_stream_from_isr(bool playing)
{
    request_t r = {REQ_WAKE, 0};
    BaseType_t woken = pdFALSE;
    want_unmuted = playing;
    xQueueSendFromISR(queue, &r, &woken);
    portYIELD_FROM_ISR(woken);
}

bool sound_park(uint32_t max_ms)
{
    xSemaphoreTake(parked_sem, 0);
    park_wanted = true;
    post(REQ_WAKE, 0);
    bool ok = xSemaphoreTake(parked_sem, pdMS_TO_TICKS(max_ms)) == pdTRUE;
    if (!ok)
    {
        debug_printf("snd: park timeout\n");
    }
    return ok;
}

void sound_volume_get(uint8_t step[2])
{
    step[0] = pub_step[PATH_SPEAKER];
    step[1] = pub_step[PATH_HEADPHONES];
}

void sound_volume_set(const uint8_t step[2])
{
    preset = true;
    preset_step[0] = step[0] <= 15 ? step[0] : 8;
    preset_step[1] = step[1] <= 15 ? step[1] : 8;
    pub_step[PATH_SPEAKER] = preset_step[0];
    pub_step[PATH_HEADPHONES] = preset_step[1];
}

void sound_ext_limits(uint8_t speaker, uint8_t headphones)
{
    if (ext_limit[PATH_SPEAKER] == speaker && ext_limit[PATH_HEADPHONES] == headphones)
    {
        return;
    }
    ext_limit[PATH_SPEAKER] = speaker;
    ext_limit[PATH_HEADPHONES] = headphones;
    post(REQ_LIMITS, 0);
}

void sound_set_step(unsigned path, unsigned step)
{
    post(REQ_SET_STEP, (uint8_t)((path & 3) << 4 | (step & 15)));
}

void sound_fade(unsigned sixteenths)
{
    post(REQ_FADE, (uint8_t)(sixteenths > 240 ? 240 : sixteenths));
}

void sound_feedback(uint16_t results, unsigned n)
{
    fb_results = results;
    fb_count = (uint8_t)(n > 16 ? 16 : n);
    post(REQ_FEEDBACK, 0);
}

unsigned sound_path(void)
{
    return pub_path;
}

unsigned sound_step(unsigned path)
{
    return path < 3 ? pub_step[path] : 0;
}

unsigned sound_limit(unsigned path)
{
    return path < 3 ? pub_limit[path] : 0;
}

uint32_t sound_ear_steps(void)
{
    return ear_steps;
}

uint32_t sound_headphone_ref(void)
{
    return headphone_ref;
}
