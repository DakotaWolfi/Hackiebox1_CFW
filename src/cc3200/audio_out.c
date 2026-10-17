/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_udma.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/i2s.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/udma.h"
#include "FreeRTOS.h"
#include "task.h"
#include "audio_out.h"
#include "lowram.h"
#include "debug.h"
#include "sound.h"
#include "dma.h"

#define BIT_CLOCK 512000u
#define WORDS (2 * AUDIO_OUT_FRAMES)

/* Stream buffers: the producer fills buffer head % N, the interrupt hands
 * buffer tail % N to a DMA half; a buffer counts as played (done) when its
 * half has completed. */
#define N_BUFFERS 6
/* Output starts (also again after running out) with the ring full: the
 * decoder of a demanding stream is barely faster than real time. */
#define START_BUFFERS N_BUFFERS
static int16_t buffers[N_BUFFERS][WORDS] LOWRAM;
static uint16_t words[N_BUFFERS]; /* words of each buffer, WORDS but the last */
static volatile uint32_t head, tail, done, underruns, interrupts;
static volatile bool streaming; /* the producer has not ended its stream */
static volatile bool playing;   /* stream output running */
static volatile bool running;   /* clocks on: set by the sound activity */
static volatile int in_use[2] = {-1, -1};

/* Transmit path on uDMA channel 5 (CC3200 SDK). The McASP's write FIFO is
 * not used: on the box it takes words from the data port but never hands
 * them to the serialiser (box test of 8868f7a); without it the McASP's
 * transmit event requests the DMA directly, one word per request. */
#define DMA_CH UDMA_CH5_I2S_TX

/* Arms a DMA half with the next buffer; false when there is none. */
static bool arm_half(int half)
{
    in_use[half] = -1;
    if (tail == head)
    {
        return false;
    }
    in_use[half] = (int)(tail % N_BUFFERS);
    tail++;
    MAP_uDMAChannelTransferSet(half ? DMA_CH | UDMA_ALT_SELECT : DMA_CH | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG,
                               (void *)buffers[in_use[half]], (void *)I2S_TX_DMA_PORT, words[in_use[half]]);
    return true;
}

void i2s_handler(void)
{
    interrupts++;
    MAP_I2SIntClear(I2S_BASE, I2S_INT_XDMA);
    if (!playing)
    {
        return;
    }
    tDMAControlTable *t = MAP_uDMAControlBaseGet();
    unsigned finished = 0;
    for (int half = 0; half < 2; half++)
    {
        uint32_t ctl = t[(DMA_CH & 0x1F) + 32 * half].ulControl;
        if (in_use[half] >= 0 && (ctl & UDMA_CHCTL_XFERMODE_M) == 0)
        {
            done++;
            in_use[half] = -1;
            finished |= 1u << half;
        }
    }
    /* A finished half is armed again only while the other one still runs;
     * an idle half is armed by the producer as soon as it commits. */
    if (in_use[0] >= 0 || in_use[1] >= 0)
    {
        for (int half = 0; half < 2; half++)
        {
            if (finished & (1u << half))
            {
                arm_half(half);
            }
        }
        /* The controller stops on a half that was idle when it got there;
         * if the producer armed it just too late, it goes on now. */
        if (!MAP_uDMAChannelIsEnabled(DMA_CH))
        {
            MAP_uDMAChannelEnable(DMA_CH);
        }
    }
    else
    {
        /* No decoded sample for the next slot: the output ends here. */
        MAP_uDMAChannelDisable(DMA_CH);
        playing = false;
        if (streaming)
        {
            underruns++;
        }
        sound_stream_from_isr(false);
    }
}

/* Reset and set-up of the McASP; with dma, the DMA is armed
 * with the queued buffers before the port starts. */
static void port_start(bool dma)
{
    MAP_IntDisable(INT_I2S);
    MAP_uDMAChannelDisable(DMA_CH);
    MAP_PRCMPeripheralReset(PRCM_I2S);
    MAP_PRCMI2SClockFreqSet(BIT_CLOCK);
    MAP_I2SConfigSetExpClk(I2S_BASE, BIT_CLOCK, BIT_CLOCK, I2S_SLOT_SIZE_16 | I2S_PORT_DMA);
    MAP_I2SSerializerConfig(I2S_BASE, I2S_DATA_LINE_0, I2S_SER_MODE_TX, I2S_INACT_LOW_LEVEL);
    if (dma)
    {
        uint32_t ctl = UDMA_SIZE_16 | UDMA_SRC_INC_16 | UDMA_DST_INC_NONE | UDMA_ARB_1;
        MAP_uDMAChannelControlSet(DMA_CH | UDMA_PRI_SELECT, ctl);
        MAP_uDMAChannelControlSet(DMA_CH | UDMA_ALT_SELECT, ctl);
        MAP_uDMAChannelAttributeDisable(DMA_CH, UDMA_ATTR_ALTSELECT);
        arm_half(0);
        arm_half(1);
        playing = true;
        MAP_uDMAChannelEnable(DMA_CH);
        /* Only the DMA-done interrupt: one per buffer half. */
        MAP_I2SIntEnable(I2S_BASE, I2S_INT_XDMA);
    }
    MAP_IntEnable(INT_I2S);
    /* Transmit high-frequency clock, bit clock, serialiser, state machine,
     * frame sync, in this order. */
    MAP_I2SEnable(I2S_BASE, I2S_MODE_TX_ONLY);
}

/* Bound for the unmute before a start: two gain settlings
 * (200 ms each) plus a chime's wait between polls. */
#define UNMUTE_WAIT_MS 600

/* Starts the stream output if it is stopped and enough buffers wait; the
 * DAC is unmuted before the port starts. Only the producer
 * starts the output, so the condition holds across the wait. */
static void start_if_due(uint32_t min)
{
    taskENTER_CRITICAL();
    bool due = !playing && head != tail && head - tail >= min;
    taskEXIT_CRITICAL();
    /* Never without the port's clock (codec not configured). */
    if (!due || !running)
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
    taskENTER_CRITICAL();
    if (!playing && head != tail)
    {
        port_start(true);
    }
    taskEXIT_CRITICAL();
}

int16_t *audio_out_buffer(void)
{
    /* A port that takes no buffer for 1 s is reported instead of blocking
     * the producer for ever. */
    for (unsigned waited = 0; head - done >= N_BUFFERS; waited += 2)
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
    words[head % N_BUFFERS] = (uint16_t)(2 * (frames < AUDIO_OUT_FRAMES ? frames : AUDIO_OUT_FRAMES));
    streaming = true;
    taskENTER_CRITICAL();
    head++;
    /* While the output runs, a half that found no buffer is armed now; the
     * controller turns to it when the running half completes. */
    if (playing)
    {
        for (int half = 0; half < 2; half++)
        {
            if (in_use[half] < 0 && (in_use[0] >= 0 || in_use[1] >= 0))
            {
                arm_half(half);
                break;
            }
        }
    }
    taskEXIT_CRITICAL();
    start_if_due(START_BUFFERS);
}

void audio_out_end(void)
{
    streaming = false;
    start_if_due(1);
}

void audio_out_flush(void)
{
    streaming = false;
    taskENTER_CRITICAL();
    bool was_playing = playing;
    playing = false;
    if (running)
    {
        port_start(false);
    }
    tail = done = head;
    in_use[0] = in_use[1] = -1;
    taskEXIT_CRITICAL();
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
    debug_verbose("audio out: %s%s, interrupts %lu, head %lu tail %lu done %lu, underruns %lu\n",
                  running ? "running" : "not started", playing ? ", playing" : "", (unsigned long)interrupts,
                  (unsigned long)head, (unsigned long)tail, (unsigned long)done, (unsigned long)underruns);
}

void audio_out_start(void)
{
    udma_init();
    MAP_IntDisable(INT_I2S);
    MAP_PinTypeI2S(PIN_50, PIN_MODE_4); /* serialiser 0 data */
    MAP_PinTypeI2S(PIN_53, PIN_MODE_2); /* bit clock */
    MAP_PinTypeI2S(PIN_63, PIN_MODE_7); /* word clock */
    MAP_PRCMPeripheralClkEnable(PRCM_I2S, PRCM_RUN_MODE_CLK);
    MAP_uDMAChannelAssign(DMA_CH);
    MAP_uDMAChannelAttributeDisable(DMA_CH, UDMA_ATTR_ALTSELECT | UDMA_ATTR_HIGH_PRIORITY | UDMA_ATTR_REQMASK);
    MAP_uDMAChannelAttributeEnable(DMA_CH, UDMA_ATTR_USEBURST);
    MAP_IntPrioritySet(INT_I2S, 3 << 5);
    port_start(false);
    running = true;
}
