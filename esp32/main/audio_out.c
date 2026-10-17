/*
 * Audio serial port of the ESP32 box: I2S0 as clock master to the codec,
 * DIN GPIO10, BCLK GPIO11, WCLK GPIO12, no MCLK (wiki pinout). 16 kHz,
 * 16-bit stereo slots: BCLK 512 kHz, from which the codec's PLL makes its
 * clock exactly as on the CC3200 box, so the shared codec
 * configuration applies unchanged.
 *
 * The clocks run continuously (the codec's tones need them); the DMA sends
 * silence while no stream data is queued. Stream output starts once
 * START_BUFFERS buffers wait, after the unmute, and ends when
 * the DMA has sent the last of the data, with the mute.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "FreeRTOS.h"
#include "task.h"
#include "audio_out.h"
#include "debug.h"
#include "port.h"
#include "sound.h"

#define RATE 16000u
/* The DMA ring is what plays on while the player waits: the queue below is
 * handed to the DMA only from the player's own calls, so a player blocked
 * in a card read (behind a download's write, which on this card takes 12 ms
 * per 4 KB and occasionally far more) has the ring and no more. 6 buffers
 * (96 ms) ran dry during downloads (device run 2026-10-14, und 8 in 24 s);
 * 24 (384 ms, 24 KB of DMA memory) cover the 250 ms an SD card may take to
 * program a write. */
#define DMA_DESC 24
#define DMA_FRAMES AUDIO_OUT_FRAMES
#define BUF_BYTES (AUDIO_OUT_FRAMES * 4u)
#define N_BUFFERS 6
#define START_BUFFERS N_BUFFERS
#define UNMUTE_WAIT_MS 600

static i2s_chan_handle_t tx;
static int16_t buffers[N_BUFFERS][2 * AUDIO_OUT_FRAMES];
static uint16_t frames_in[N_BUFFERS];
static volatile uint32_t head, tail; /* queued, not yet written to the DMA */
static volatile bool streaming, playing, running, stalled;
static volatile uint32_t written_bytes; /* since the output started */
static volatile uint32_t sent_buffers;  /* DMA buffers sent since then */
static volatile uint32_t underruns, interrupts;

/* A DMA buffer has been sent (interrupt). The output has ended when every
 * buffer holding data has gone out: the first write lands in the buffer
 * after the one sending at the start, hence the one extra. */
static bool on_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    (void)h;
    (void)e;
    (void)ctx;
    interrupts++;
    if (!playing)
    {
        return false;
    }
    sent_buffers++;
    uint32_t used = (written_bytes + BUF_BYTES - 1) / BUF_BYTES;
    if (sent_buffers > used && tail == head)
    {
        playing = false;
        if (streaming)
        {
            underruns++;
        }
        sound_stream_from_isr(false);
    }
    return false;
}

/* Hands queued buffers to the DMA; blocks while its ring is full. */
static void drain(void)
{
    while (tail != head)
    {
        unsigned i = tail % N_BUFFERS;
        size_t n = 0;
        size_t bytes = (size_t)frames_in[i] * 4u;
        if (i2s_channel_write(tx, buffers[i], bytes, &n, pdMS_TO_TICKS(1000)) != ESP_OK || n != bytes)
        {
            stalled = true;
            return;
        }
        CRITICAL_ENTER();
        written_bytes += (uint32_t)n;
        tail++;
        CRITICAL_EXIT();
    }
}

/* Starts the stream output when enough buffers wait; the DAC is unmuted
 * before. */
static void start_if_due(uint32_t min)
{
    if (playing || !running || head - tail < min || head == tail)
    {
        return;
    }
    TickType_t t0 = xTaskGetTickCount();
    bool ok = sound_stream_start(UNMUTE_WAIT_MS);
    TickType_t dt = xTaskGetTickCount() - t0;
    if (!ok || dt > 100)
    {
        debug_printf("snd: unmute wait %lu ms%s\n", (unsigned long)dt, ok ? "" : " timeout");
    }
    CRITICAL_ENTER();
    written_bytes = 0;
    sent_buffers = 0;
    playing = true;
    CRITICAL_EXIT();
    drain();
}

int16_t *audio_out_buffer(void)
{
    if (stalled)
    {
        stalled = false;
        return NULL; /* the port took nothing for 1 s */
    }
    for (unsigned waited = 0; head - tail >= N_BUFFERS; waited += 2)
    {
        if (waited >= 1000)
        {
            return NULL;
        }
        vTaskDelay(2);
    }
    return buffers[head % N_BUFFERS];
}

void audio_out_commit(void)
{
    audio_out_commit_frames(AUDIO_OUT_FRAMES);
}

void audio_out_commit_frames(unsigned frames)
{
    if (!frames)
    {
        return;
    }
    frames_in[head % N_BUFFERS] = (uint16_t)(frames < AUDIO_OUT_FRAMES ? frames : AUDIO_OUT_FRAMES);
    streaming = true;
    head++;
    if (playing)
    {
        drain();
    }
    else
    {
        start_if_due(START_BUFFERS);
    }
}

void audio_out_end(void)
{
    streaming = false;
    start_if_due(1);
    if (playing)
    {
        drain();
    }
}

/* Silence in every DMA buffer, so that nothing queued plays on. */
static void silence(void)
{
    static const int16_t zero[2 * AUDIO_OUT_FRAMES];
    i2s_channel_disable(tx);
    size_t n;
    for (int i = 0; i < DMA_DESC; i++)
    {
        i2s_channel_preload_data(tx, zero, sizeof zero, &n);
    }
    i2s_channel_enable(tx);
}

void audio_out_flush(void)
{
    streaming = false;
    bool was_playing = playing;
    CRITICAL_ENTER();
    playing = false;
    tail = head;
    CRITICAL_EXIT();
    if (running)
    {
        silence();
    }
    if (was_playing)
    {
        sound_stream(false);
    }
}

bool audio_out_wait_idle(uint32_t ms)
{
    for (uint32_t waited = 0; playing || tail != head; waited += 5)
    {
        if (waited >= ms)
        {
            return false;
        }
        vTaskDelay(5);
    }
    return true;
}

uint32_t audio_out_underruns(void)
{
    return underruns;
}

bool audio_out_running(void)
{
    return running;
}

void audio_out_report(void)
{
    debug_verbose("audio out: %s%s, interrupts %lu, head %lu tail %lu, underruns %lu\n",
                  running ? "running" : "not started", playing ? ", playing" : "", (unsigned long)interrupts,
                  (unsigned long)head, (unsigned long)tail, (unsigned long)underruns);
}

void audio_out_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC;
    cc.dma_frame_num = DMA_FRAMES;
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK)
    {
        debug_printf("snd: I2S channel FAILED\n");
        return;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = GPIO_NUM_11,
                .ws = GPIO_NUM_12,
                .dout = GPIO_NUM_10,
                .din = I2S_GPIO_UNUSED,
            },
    };
    i2s_event_callbacks_t cb = {.on_sent = on_sent};
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_register_event_callback(tx, &cb, NULL) != ESP_OK)
    {
        debug_printf("snd: I2S set-up FAILED\n");
        return;
    }
    i2s_channel_enable(tx);
    running = true;
}
