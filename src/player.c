/*
 * Playback control and content playback: stream structure and decoding,
 * states, resume, chapters, winding, end of content, content file checks
 * and reading.
 *
 * Also sound prompts, playing while
 * downloading, the outdated mark after an invalid block, the language chapter
 * and the held-back figure
 * events (conditions a "syncing" and b via
 * cloud_holds_figures). Not yet: prompts waiting while a synchronisation
 * runs or the box shuts down.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "ff.h"
#include "opus.h"
#include "port.h"
#include "audio_out.h"
#include "cloud.h"
#include "debug.h"
#include "journal.h"
#include "ledui.h"
#include "monitor.h"
#include "net.h"
#include "lowram.h"
#include "player.h"
#include "power.h"
#include "pwrstate.h"
#include "rtnl.h"
#include "settings.h"
#include "sfx.h"
#include "sdfiles.h"
#include "sound.h"
#include "taf.h"
#include "tick.h"
#include "xplay.h"

#define FRAME_MAX 320          /* 20 ms at 16 kHz */
#define PACKET_MAX_FRAMES 1920 /* 120 ms */
#define PACKET_MAX 1536u

/* --- events --- */

/* Figure presence, sound-prompt requests, sync requests and sync results
 * are kept as state written by the posting activities; the queue carries
 * the gestures and wake-ups only, so that a full queue during a long
 * blocking phase (download wait, prompt, archive unpacking) loses no figure
 * event, prompt or sync result. */
typedef enum
{
    EV_WAKE,
    EV_CHAPTER,
    EV_WIND,
    EV_RESTART,
    EV_EAR,
    EV_EARS_SWITCH,
    EV_LOCKED
} ev_kind_t;

typedef struct
{
    uint8_t kind;
    uint8_t forward; /* EV_LOCKED: gesture | source << 4 */
    uint32_t at;     /* EV_RESTART: tick of the slap */
} event_t;

#define QUEUE_LEN 8
static StaticQueue_t queue_mem;
static uint8_t queue_buf[QUEUE_LEN * sizeof(event_t)];
static QueueHandle_t queue;

static void post(const event_t *e)
{
    if (xQueueSend(queue, e, 0) != pdTRUE && e->kind != EV_WAKE)
    {
        debug_printf("pl: queue full, event %u lost\n", e->kind);
    }
}

/* Best effort: when the queue is full the player has events to take and
 * looks at the state then. */
static void wake(void)
{
    event_t e = {.kind = EV_WAKE};
    post(&e);
}

/* Written under a critical section by the tag activity. */
static struct
{
    uint32_t changes, removals;
    bool present, has_token;
    uint8_t uid[8], token[32];
} fig_in;

void player_placed(const uint8_t uid[8], const uint8_t *token)
{
    CRITICAL_ENTER();
    fig_in.present = true;
    memcpy(fig_in.uid, uid, 8);
    fig_in.has_token = token != NULL;
    if (token)
    {
        memcpy(fig_in.token, token, 32);
    }
    fig_in.changes++;
    CRITICAL_EXIT();
    wake();
}

void player_removed(void)
{
    CRITICAL_ENTER();
    fig_in.present = false;
    fig_in.changes++;
    fig_in.removals++;
    CRITICAL_EXIT();
    wake();
}

void player_chapter(bool forward)
{
    event_t e = {.kind = EV_CHAPTER, .forward = forward};
    post(&e);
}

void player_wind(bool forward)
{
    event_t e = {.kind = EV_WIND, .forward = forward};
    post(&e);
}

void player_restart_gesture(uint32_t at)
{
    event_t e = {.kind = EV_RESTART, .at = at};
    post(&e);
}

void player_ear_level(bool big)
{
    event_t e = {.kind = EV_EAR, .forward = big};
    post(&e);
}

void player_ears_switch(void)
{
    event_t e = {.kind = EV_EARS_SWITCH};
    post(&e);
}

void player_locked(unsigned gesture, unsigned source)
{
    event_t e = {.kind = EV_LOCKED, .forward = (uint8_t)(gesture | source << 4)};
    post(&e);
}

/* The extension's events that do not depend on the playback state; true
 * when e was one. */
static bool ext_event(const event_t *e)
{
    switch (e->kind)
    {
    case EV_EAR:
        xp_ear_level(e->forward);
        return true;
    case EV_EARS_SWITCH:
        xp_ears_switch();
        return true;
    case EV_LOCKED:
        xp_locked(e->forward & 15, e->forward >> 4);
        return true;
    default:
        return e->kind == EV_WAKE;
    }
}

/* --- figure, prompt and sync bookkeeping --- */

static bool figure_present, figure_has_token, figure_changed, figure_removed;
static uint8_t figure_uid[8], figure_token[32];
static uint32_t fig_changes_seen, fig_removals_seen;
static bool startup_hold = true; /* until prompt 0 has ended (condition c) */
static uint8_t prompt_fifo[8], prompt_wait[8];
static volatile unsigned prompt_head, prompt_tail;
static volatile bool sync_req_in, result_in, result_user;
static volatile int result_error;
static bool sync_wanted;
static bool prompt_mode, prompt_cut, dl_starved;
static volatile bool power_off_wanted, power_off_done; /* power-off step 3 */
static volatile bool stop_all_req, stop_prompt_req;
static volatile bool switch_off; /* F9, until the box is off */
/* Remote control: requests of the log activity. */
static volatile bool rc_pause, rc_continue, rc_stop, rc_play;
static uint8_t rc_uid[8];
static volatile bool paused, preparing, remote_playing;
static volatile uint32_t pub_chapter;
static bool stopped_all;

/* Figure events wait during an online round or a sync and
 * during the setup search and setup mode. */
static bool figures_held(void)
{
    return cloud_holds_figures() || net_setup_active();
}

void player_stop_all(void)
{
    stop_all_req = true;
    wake();
}

void player_stop_prompt(void)
{
    stop_prompt_req = true;
    wake();
}

/* Takes over the figure state. Several changes count as one;
 * figure_removed notes that a removal was among them. */
static void figure_take(void)
{
    CRITICAL_ENTER();
    bool changed = fig_in.changes != fig_changes_seen;
    if (changed)
    {
        if (fig_in.removals != fig_removals_seen)
        {
            figure_removed = true;
        }
        fig_changes_seen = fig_in.changes;
        fig_removals_seen = fig_in.removals;
        figure_present = fig_in.present;
        memcpy(figure_uid, fig_in.uid, 8);
        figure_has_token = fig_in.has_token;
        memcpy(figure_token, fig_in.token, 32);
    }
    CRITICAL_EXIT();
    if (changed)
    {
        figure_changed = true;
    }
}

static void prompt_push(unsigned nn, bool wait)
{
    CRITICAL_ENTER();
    if (prompt_head - prompt_tail < sizeof prompt_fifo)
    {
        prompt_fifo[prompt_head % sizeof prompt_fifo] = (uint8_t)nn;
        prompt_wait[prompt_head % sizeof prompt_fifo] = wait;
        prompt_head++;
    }
    CRITICAL_EXIT();
}

static bool prompt_pending(void)
{
    return prompt_head != prompt_tail;
}

void player_prompt(unsigned nn, bool wait)
{
    prompt_push(nn, wait);
    wake();
}

void player_sync_request(void)
{
    sync_req_in = true;
    wake();
}

/* A result for a sync the user waits for is not replaced by that of an
 * automatic one. */
static void sync_led_back(void);

void player_sync_result(int error, bool user)
{
    if (!user)
    {
        sync_led_back();
    }
    CRITICAL_ENTER();
    if (user || !result_in || !result_user)
    {
        result_error = error;
        result_user = user;
    }
    result_in = true;
    CRITICAL_EXIT();
    wake();
}

/* A sync request while the player is busy (only the signal tone of an
 * accepted request is queued). */
static void sync_request_busy(void)
{
    if (sync_req_in)
    {
        sync_req_in = false;
        debug_printf("pl: sync while busy (6146)\n");
    }
}

/* --- resume record --- */

static struct
{
    bool valid;
    bool tag_only; /* after a wake-up: only the 12-bit tag */
    uint16_t tag;
    uint8_t uid[8];
    uint32_t block; /* next block to be read */
} resume;

/* The resume record names this figure. */
static bool resume_names(const uint8_t uid[8])
{
    if (!resume.valid)
    {
        return false;
    }
    if (resume.tag_only)
    {
        return (uid[0] | (uid[1] & 15) << 8) == resume.tag;
    }
    return !memcmp(resume.uid, uid, 8);
}

uint32_t player_resume_get(void)
{
    return resume.valid ? pwrstate_resume(resume.uid, resume.block) : 0;
}

void player_resume_set(uint32_t r)
{
    resume.valid = r != 0;
    resume.tag_only = true;
    resume.tag = r & 0xFFF;
    resume.block = (r >> 12) * 2;
}

/* --- the content being played --- */

static FIL file;
static taf_header_t hdr;
static uint8_t block[TAF_BLOCK];
/* The block at read_pos read ahead while the output is full, so that a
 * download's card write (up to 630 ms on some cards) holds up the reading
 * ahead and not the decoder. The memory is the cloud activity's, lent
 * while a content download writes (player_ahead_begin/end). */
static uint8_t *volatile ahead;
static uint32_t ahead_pos; /* its offset */
static unsigned ahead_len; /* its length, 0 when there is none */
static bool ahead_on;      /* while a content plays */
static volatile bool want_card, reading, writing;
static uint32_t size;       /* Z */
static uint32_t read_pos;   /* R: offset after the last block read */
static uint32_t jump_start; /* J: jump start offset, 0 when none */
static uint64_t end_pos;    /* end of the language chapter, else Z */
static uint32_t started_ms, last_jump_ms;
static uint8_t uid_playing[8];
static volatile bool playing;
static bool stop_request;
static const char *stop_why; /* the reason of the stop, for the log */
#define STOP(why) (stop_request = true, stop_why = (why))
/* Playing while the content downloads. */
static volatile bool downloading;
static uint32_t dl_serial, retry_at, retry_wait;
static bool dl_complete; /* completion seen (chime 7) */

/* the end of an automatic sync (log-host command 6, the online
 * round) hands the LED back at once; the result itself waits for the idle
 * loop, and while content plays or is paused the LED stayed blue until
 * then. Runs in the cloud activity: the flags are only read. */
static void sync_led_back(void)
{
    if (playing && !paused && !prompt_mode)
    {
        ledui_request(downloading && !dl_complete ? LED_CONTENT_DOWNLOAD : LED_READY_PLAYING);
    }
    else
    {
        ledui_request(LED_READY_IDLE);
    }
}
static uint32_t dl_audio_id; /* audio_id as last read for the figure, 0 when none */
static bool range_refused;   /* 416 to a resumed request */
static uint8_t token_playing[32];
static bool has_token_playing;

static volatile bool busy;

/* The box activity: content prepared or playing, prompt. */
static void set_busy(bool b)
{
    busy = b;
    monitor_box_activity(b);
}

bool player_playing(void)
{
    return playing;
}

bool player_busy(void)
{
    return busy;
}

static void poll_events(void);

/* --- stream state --- */

static uint8_t dec_mem[17984] __attribute__((aligned(8))); /* opus_decoder_get_size(2) = 17932 on the box */
static bool dec_too_small;                                 /* nothing is decoded then */
static OpusDecoder *const dec = (OpusDecoder *)dec_mem;
static uint8_t packet[PACKET_MAX] LOWRAM; /* written before use */
static int16_t pcm[2 * FRAME_MAX];

static struct
{
    bool started;  /* a page with sequence number 0 was seen */
    bool decoding; /* identification header accepted */
    bool dead;     /* undecodable or ended: discard until page 0 */
    bool ending;   /* end of stream at the end of this block */
    uint32_t serial, last_seq;
    uint32_t preskip;
    int16_t gain;
    unsigned plen;
    bool skip_continued; /* drop the rest of a packet begun before a jump */
    unsigned page0_packets;
} st;

static struct
{
    uint32_t packets, frames, decode_ms, max_ms, read_max, ahead;
} stats;

uint32_t player_decode_ms;

/* Output: stream buffer being filled. */
static int16_t *out;
static unsigned out_frames;

static void read_ahead(void);

static void emit(const int16_t *s, unsigned frames)
{
    while (frames && !stop_request)
    {
        if (!out)
        {
            read_ahead();
            out = audio_out_buffer();
            if (!out)
            {
                debug_printf("pl: output takes none\n");
                audio_out_report();
                STOP("output");
                return;
            }
            out_frames = 0;
        }
        unsigned n = AUDIO_OUT_FRAMES - out_frames;
        if (n > frames)
        {
            n = frames;
        }
        memcpy(out + 2 * out_frames, s, 4 * n);
        out_frames += n;
        s += 2 * n;
        frames -= n;
        if (out_frames == AUDIO_OUT_FRAMES)
        {
            audio_out_commit();
            out = NULL;
        }
    }
}

/* The last buffer goes out with the frames it holds: no generated samples. */
static void emit_end(void)
{
    if (out && out_frames)
    {
        audio_out_commit_frames(out_frames);
    }
    out = NULL;
    audio_out_end();
}

static void undecodable(const char *why)
{
    if (!st.dead)
    {
        debug_printf("pl: undecodable %s, page 0\n", why);
    }
    st.dead = true;
    st.decoding = false;
}

static void decoder_reset(void)
{
    opus_decoder_init(dec, 16000, 2);
    if (st.gain)
    {
        opus_decoder_ctl(dec, OPUS_SET_GAIN(st.gain));
    }
}

static bool header_ok(const uint8_t *p, unsigned len)
{
    if (len < 19 || memcmp(p, "OpusHead", 8) || (p[8] & 0xF0) || p[9] != 2)
    {
        return false;
    }
    unsigned need = 19;
    if (p[18] != 0)
    {
        need = 21 + 2;
        if (len < need)
        {
            return false;
        }
        unsigned streams = p[19], coupled = p[20];
        if (streams < 1 || coupled > streams || streams + coupled > 255)
        {
            return false;
        }
        for (int c = 0; c < 2; c++)
        {
            if (p[21 + c] > streams + coupled && p[21 + c] != 255)
            {
                return false;
            }
        }
    }
    if (p[8] <= 1 && len != need)
    {
        return false;
    }
    return true;
}

/* Decoded frame of L sample frames in pcm: pre-skip, output. */
static void frame_out(int l)
{
    stats.frames += (uint32_t)l;
    if (st.preskip >= (uint32_t)l)
    {
        st.preskip -= (uint32_t)l;
        return;
    }
    if (st.preskip)
    {
        unsigned p = st.preskip;
        for (unsigned i = 0; i < (unsigned)l - p; i++)
        {
            pcm[i] = pcm[2 * p + i];
        }
        st.preskip = 0;
    }
    emit(pcm, (unsigned)l);
}

/* the frames of a packet one after the other. */
static void audio_packet(const uint8_t *p, unsigned len)
{
    if (len == 0)
    {
        undecodable("empty packet");
        return;
    }
    if ((p[0] >> 3) < 16)
    {
        undecodable("not CELT-only");
        return;
    }
    const unsigned char *frames[48];
    opus_int16 sizes[48];
    unsigned char toc;
    int n = opus_packet_parse(p, (opus_int32)len, &toc, frames, sizes, NULL);
    int spf = opus_packet_get_samples_per_frame(p, 16000);
    if (n <= 0 || n * spf > PACKET_MAX_FRAMES)
    {
        undecodable("malformed packet");
        return;
    }
    uint32_t t0 = tick_ms();
    for (int i = 0; i < n && !st.dead; i++)
    {
        int l;
        uint32_t td = tick_ms();
        if (sizes[i] <= 1)
        {
            l = opus_decode(dec, NULL, 0, pcm, spf, 0); /* concealment */
        }
        else if (n == 1 && (toc & 3) == 0)
        {
            l = opus_decode(dec, p, (opus_int32)len, pcm, FRAME_MAX, 0);
        }
        else
        {
            /* A one-frame packet in place: its TOC byte (code 0) is put in
             * front of the frame data for the call and the byte that was
             * there is put back. The frames lie after the packet's TOC
             * byte, so frames[i] - 1 is always inside the packet. */
            uint8_t *t = (uint8_t *)frames[i] - 1, keep = *t;
            *t = toc & 0xFC;
            l = opus_decode(dec, t, sizes[i] + 1, pcm, FRAME_MAX, 0);
            *t = keep;
        }
        player_decode_ms += tick_ms() - td;
        if (l < 0)
        {
            undecodable("decoder error");
            return;
        }
        frame_out(l);
    }
    uint32_t dt = tick_ms() - t0;
    stats.decode_ms += dt;
    if (dt > stats.max_ms)
    {
        stats.max_ms = dt;
    }
    stats.packets++;
}

static void packet_done(uint32_t seq, const uint8_t *p, unsigned len)
{
    if (seq == 0)
    {
        if (st.page0_packets++ > 0)
        {
            undecodable("second packet on page 0");
            return;
        }
        if (!header_ok(p, len))
        {
            undecodable("identification header");
            return;
        }
        st.preskip = (uint32_t)(p[10] | p[11] << 8);
        st.gain = (int16_t)(p[16] | p[17] << 8);
        decoder_reset();
        st.decoding = true;
        return;
    }
    if (seq == 1)
    {
        return; /* comment header */
    }
    if (!st.decoding)
    {
        undecodable("audio without a decoder");
        return;
    }
    audio_packet(p, len);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* record 6803 at the start of a stream; its own frame, off the
 * decoder's path. The count also takes the page-sequence gaps. */
static uint32_t streams_started;

static void __attribute__((noinline)) log_stream_start(uint32_t serial)
{
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)serial);
    log_arg_num(&a, (int32_t)++streams_started);
    log_fn(LOG_INFO, 20, 6803, 0, &a);
}

static void process_page(const uint8_t *pg)
{
    uint8_t flags = pg[5];
    uint32_t serial = le32(pg + 14), seq = le32(pg + 18);
    unsigned segs = pg[26];

    if (seq == 0)
    {
        /* a new stream. */
        bool ending = st.ending;
        memset(&st, 0, sizeof st);
        st.started = true;
        st.serial = serial;
        st.ending = ending;
        log_stream_start(serial);
    }
    else
    {
        if (flags & 0x02)
        {
            undecodable("beginning-of-stream flag");
            return;
        }
        if (!st.started || st.dead)
        {
            return;
        }
        if (serial != st.serial)
        {
            undecodable("serial number");
            return;
        }
        if (seq != st.last_seq + 1)
        {
            streams_started++;
            if (st.decoding)
            {
                decoder_reset();
            }
            st.preskip = 0;
            st.plen = 0;
        }
    }
    st.last_seq = seq;

    if (!(flags & 0x01))
    {
        st.plen = 0;
        st.skip_continued = false;
    }
    else if (st.plen == 0)
    {
        st.skip_continued = true;
    }

    /* A packet that begins and ends on this page is decoded where it lies
     * in the block (its padding can make it far longer than a frame, as on
     * the last page of a stream); only a packet continued from the
     * previous page is assembled in the packet buffer. */
    const uint8_t *data = pg + 27 + segs, *start = data;
    bool here = st.plen == 0;
    for (unsigned i = 0; i < segs && !st.dead && !stop_request; i++)
    {
        unsigned l = pg[27 + i];
        if (!here && st.plen + l <= PACKET_MAX)
        {
            memcpy(packet + st.plen, data, l);
        }
        st.plen += l;
        data += l;
        if (l == 255 && i == segs - 1 && here)
        {
            /* continues on the next page: keep what fits */
            unsigned n = st.plen <= PACKET_MAX ? st.plen : PACKET_MAX;
            memcpy(packet, start, n);
        }
        if (l < 255)
        {
            if (st.skip_continued)
            {
                st.skip_continued = false;
            }
            else if (here)
            {
                packet_done(seq, start, st.plen);
            }
            else if (st.plen > PACKET_MAX)
            {
                undecodable("packet too long");
            }
            else
            {
                packet_done(seq, packet, st.plen);
            }
            st.plen = 0;
            start = data;
            here = true;
            /* Removal must stop the sound within 200 ms. */
            poll_events();
        }
    }
    if ((flags & 0x04) && seq >= 2 && segs > 0)
    {
        if (!st.ending)
        {
            debug_printf("pl: end of stream, page %lu (6889)\n",
                         (unsigned long)seq);
        }
        st.ending = true;
    }
}

/* --- jumps --- */

/* 64 bits: (P - 1) x 4096 can exceed 32 bits, and such an offset is
 * beyond the file. */
static uint64_t chapter_offset(uint32_t page)
{
    return page <= 2 ? TAF_BLOCK : (uint64_t)(page - 1) * TAF_BLOCK;
}

static int current_chapter(void)
{
    for (unsigned i = 0; i < hdr.chapters; i++)
    {
        uint64_t lo = (uint64_t)hdr.chapter[i] * TAF_BLOCK;
        uint64_t hi = i + 1 < hdr.chapters ? (uint64_t)hdr.chapter[i + 1] * TAF_BLOCK : size;
        if (lo < read_pos && read_pos <= hi)
        {
            return (int)i;
        }
    }
    return -1;
}

/* While the content still downloads, a chapter or wind request plays its
 * chime and jumps only into the part already on the card (the original
 * box was heard to play the chime). */
static bool jump_to(uint32_t offset, const char *what)
{
    if (downloading && !cloud_dl.done && (uint64_t)offset + TAF_BLOCK > cloud_dl.readable)
    {
        debug_printf("pl: jump %lu not loaded (%lu) (6218)\n", (unsigned long)offset,
                     (unsigned long)cloud_dl.readable);
        return false;
    }
    debug_printf("pl: jump %lu -> %lu\n", (unsigned long)read_pos, (unsigned long)offset);
    debug_verbose("pl: (%s)\n", what);
    read_pos = offset;
    jump_start = 0; /* J is used only once */
    return true;
}

/* --- shuffle --- */

static struct
{
    bool on;       /* this playback follows a shuffle order */
    bool finished; /* its last chapter has been played */
    uint32_t seed;
    uint8_t n, index;
    uint8_t order[TAF_MAX_CHAPTERS];
    uint64_t chap_end; /* end of the chapter being played */
} shuf;

/* Reading range of chapter k: from its start offset to that
 * of the next chapter in the file, or Z. */
static uint64_t chap_start(unsigned k)
{
    return chapter_offset(hdr.chapter[k]);
}

static uint64_t chap_end(unsigned k)
{
    return k + 1 < hdr.chapters ? chapter_offset(hdr.chapter[k + 1]) : size;
}

/* A new order (from the previous seed and the uptime). */
static void shuffle_new(uint32_t prev)
{
    uint32_t t = tick_ms();
    shuf.seed = (prev * 0x9E3779B1u) ^ t ^ (t << 16) ^ 0x5BD1E995u;
    shuf.index = 0;
    shuf.finished = false;
    ext_shuffle(shuf.seed, shuf.n, shuf.order);
    debug_verbose("player: new shuffle order, seed %08lx, first chapter %u\n", (unsigned long)shuf.seed,
                  shuf.order[0] + 1);
}

/* Jumps to the start of the chapter at shuffle index t; back: not below
 * 12288. False when the offset is beyond the file. */
static bool shuffle_jump(unsigned t, bool back, const char *what)
{
    uint64_t off = chap_start(shuf.order[t]);
    if (back && off < 3 * TAF_BLOCK)
    {
        off = 3 * TAF_BLOCK;
    }
    if (off >= size)
    {
        return false;
    }
    char w[40];
    snprintf(w, sizeof w, "%s, chapter %u (order %u)", what, shuf.order[t] + 1, t + 1);
    if (!jump_to((uint32_t)off, w))
    {
        return false;
    }
    shuf.index = (uint8_t)t;
    shuf.chap_end = chap_end(shuf.order[t]);
    return true;
}

/* the last block of the chapter has been read; false at the
 * end of the order. */
static bool shuffle_next(void)
{
    if (shuf.index + 1u >= shuf.n)
    {
        shuf.finished = true;
        debug_verbose("pl: shuffle done\n");
        return false;
    }
    if (!shuffle_jump(shuf.index + 1u, false, "shuffle"))
    {
        shuf.finished = true;
        return false;
    }
    return true;
}

static void chapter(bool forward)
{
    sound_chime(forward ? CHIME_CHAPTER_FORWARD : CHIME_CHAPTER_BACK);
    uint32_t now = tick_ms();
    if (shuf.on)
    {
        /* along the order, wrapping at its ends */
        unsigned i = shuf.index, n = shuf.n, t;
        uint32_t ref = last_jump_ms > started_ms ? last_jump_ms : started_ms;
        if (forward)
        {
            t = i + 1 < n ? i + 1 : 0;
        }
        else if (i == 0)
        {
            t = n - 1;
        }
        else
        {
            t = now - ref < 11000 ? i - 1 : i;
        }
        if (shuffle_jump(t, !forward, forward ? "chapter forward" : "chapter back"))
        {
            last_jump_ms = now;
        }
        return;
    }
    int i = current_chapter();
    if (i < 0)
    {
        debug_printf("pl: no current chapter\n");
        return;
    }
    int n = (int)hdr.chapters, target;
    if (forward)
    {
        target = i + 1 < n ? i + 1 : 0;
    }
    else if (i == 0)
    {
        target = n - 1;
    }
    else
    {
        uint32_t ref = last_jump_ms > started_ms ? last_jump_ms : started_ms;
        target = now - ref < 11000 ? i - 1 : i;
    }
    uint64_t off = chapter_offset(hdr.chapter[target]);
    if (!forward && off < 3 * TAF_BLOCK)
    {
        off = 3 * TAF_BLOCK;
    }
    if (off >= size)
    {
        return;
    }
    char what[40];
    snprintf(what, sizeof what, "chapter %d -> %d", i + 1, target + 1);
    if (jump_to((uint32_t)off, what))
    {
        last_jump_ms = now;
    }
}

/* a chapter-back request to the beginning of the content, with
 * shuffle to the first chapter of a new order. */
static void restart(void)
{
    sound_chime(CHIME_CHAPTER_BACK);
    uint32_t now = tick_ms();
    if (shuf.on)
    {
        shuffle_new(shuf.seed);
        if (shuffle_jump(0, true, "restart"))
        {
            last_jump_ms = now;
        }
        return;
    }
    uint64_t off = hdr.chapters ? chapter_offset(hdr.chapter[0]) : TAF_BLOCK;
    if (off < 3 * TAF_BLOCK)
    {
        off = 3 * TAF_BLOCK;
    }
    if (off >= size)
    {
        return;
    }
    if (jump_to((uint32_t)off, "restart"))
    {
        last_jump_ms = now;
    }
}

static void wind(bool forward)
{
    sound_chime(forward ? CHIME_WIND_FORWARD : CHIME_WIND_BACK);
    uint32_t p = read_pos, np;
    if (forward)
    {
        uint64_t f = ((uint64_t)p + 40960) & ~(uint64_t)(TAF_BLOCK - 1);
        if (f >= size)
        {
            return;
        }
        np = (uint32_t)f;
    }
    else
    {
        np = p <= 40960 ? 0 : (p - 40960) & ~(TAF_BLOCK - 1);
        if (np < 3 * TAF_BLOCK)
        {
            np = 3 * TAF_BLOCK;
        }
    }
    (void)jump_to(np, forward ? "wind forward" : "wind back");
}

static const char *gesture_name(const event_t *e)
{
    return e->kind == EV_CHAPTER ? "chapter" : e->kind == EV_WIND ? "wind"
                                                                  : "restart";
}

/* a request without an audible effect is only logged. */
static void gesture(const event_t *e)
{
    if (ext_event(e))
    {
        return;
    }
    if (prompt_mode)
    {
        return; /* Prompt: ignored */
    }
    if (e->kind == EV_CHAPTER)
    {
        chapter(e->forward);
    }
    else if (e->kind == EV_WIND)
    {
        wind(e->forward);
    }
    else if (tick_ms() - e->at <= 3000)
    {
        restart();
    }
    else
    {
        debug_verbose("pl: restart late (6218)\n");
    }
}

/* Figure changes while the content plays: acted on once no hold applies; a
 * prompt holds them until it has ended. */
static void figure_while_playing(void)
{
    figure_take();
    if (!figure_changed || prompt_mode)
    {
        return;
    }
    if (figures_held())
    {
        figure_removed = false; /* the state at the end of the hold counts */
        return;
    }
    figure_changed = false;
    if (figure_removed || !figure_present)
    {
        STOP("removed");
        ledui_request(LED_ACTIVITY);
        log_event(EVT_PLAYBACK_STOPPED, NULL);
        xp_remove();
        /* placed again meanwhile: acted on once the box is idle */
        figure_changed = figure_present;
    }
    else if (remote_playing || paused)
    {
        /* command 71: a placed figure ends it and is
         * handled as usual */
        STOP("placed");
        figure_changed = true;
        log_event(EVT_PLAYBACK_STOPPED, NULL);
        xp_remove();
    }
    else
    {
        debug_printf("pl: placed while playing (6157)\n");
    }
    figure_removed = false;
}

static void download_watch(void);

/* playback held at the current position until continued, or
 * until a gesture moves the position (it is carried out and playback goes
 * on). The box counts as idle meanwhile; a removal, a placement, a stop,
 * a prompt or the power-off end the pause without continuing. */
static void pause_loop(void)
{
    debug_verbose("pl: paused at %lu\n", (unsigned long)read_pos);
    want_card = false;
    if (out && out_frames)
    {
        audio_out_commit_frames(out_frames); /* what was decoded plays out */
    }
    out = NULL;
    audio_out_end();
    audio_out_wait_idle(500);
    rc_continue = false;
    paused = true;
    set_busy(false);
    ledui_request(LED_READY_IDLE);
    xp_timer_hold(true);
    while (!stop_request)
    {
        monitor_mark(ACT_SD);
        if (downloading)
        {
            download_watch();
        }
        figure_while_playing();
        if (stop_request || power_off_wanted || stop_all_req || rc_stop || switch_off || prompt_pending())
        {
            break;
        }
        if (rc_continue)
        {
            break;
        }
        sync_request_busy();
        event_t e;
        if (xQueueReceive(queue, &e, pdMS_TO_TICKS(100)) == pdTRUE && !ext_event(&e))
        {
            uint32_t before = read_pos;
            gesture(&e);
            if (read_pos != before)
            {
                break;
            }
        }
    }
    rc_continue = false;
    paused = false;
    set_busy(true);
    xp_timer_hold(false);
    if (!stop_request && !switch_off)
    {
        ledui_request(downloading && !dl_complete ? LED_CONTENT_DOWNLOAD : LED_READY_PLAYING);
    }
    debug_verbose("pl: pause ends\n");
}

/* Events while the content plays (state Content). */
static void poll_events(void)
{
    event_t e;
    if (stop_request)
    {
        return;
    }
    figure_while_playing();
    if (power_off_wanted)
    {
        STOP("power-off");
    }
    if (stop_prompt_req)
    {
        stop_prompt_req = false;
        if (prompt_mode)
        {
            STOP("net"); /* only a prompt */
        }
    }
    if (stop_all_req && !stop_request)
    {
        /* content and prompt stop; a figure still there is
         * handled again when setup mode ends */
        stop_all_req = false;
        STOP("setup");
        stopped_all = true;
        if (!prompt_mode)
        {
            log_event(EVT_PLAYBACK_STOPPED, NULL);
            figure_changed = figure_present;
        }
    }
    if ((rc_stop || switch_off) && !stop_request && !prompt_mode)
    {
        /* command 67: as on removal; the figure counts as
         * removed until it is placed again. F9: the same, and a
         * running sleep timer ends. */
        rc_stop = false;
        STOP(switch_off ? "F9" : "remote");
        log_event(EVT_PLAYBACK_STOPPED, NULL);
        if (switch_off)
        {
            xp_power_off();
        }
        else
        {
            xp_remove();
        }
        figure_changed = false;
    }
    if (xp_poll() && !stop_request)
    {
        /* the sleep timer has run out: as on removal, then
         * the power-off */
        STOP("sleep timer");
        log_event(EVT_PLAYBACK_STOPPED, NULL);
        ledui_request(LED_ACTIVITY);
        power_off_soon();
    }
    if (!prompt_mode && prompt_pending() && !stop_request)
    {
        /* the content stops as on removal; the figure is
         * taken as newly placed after the prompt. */
        STOP("prompt");
        prompt_cut = true;
        ledui_request(LED_READY_IDLE);
    }
    sync_request_busy();
    while (!stop_request && xQueueReceive(queue, &e, 0) == pdTRUE)
    {
        gesture(&e);
    }
    if (rc_pause && !stop_request && !prompt_mode)
    {
        rc_pause = false;
        pause_loop();
        poll_events(); /* what ended the pause */
    }
}

/* --- playing a content file --- */

static void download_watch(void);

/* an audio_id 1 file played as it downloads reads nothing
 * while J is pending, below Z and not yet downloaded up to J + 65536. */
static void wait_language_chapter(void)
{
    while (downloading && jump_start && jump_start < size && hdr.audio_id == 1 && !stop_request &&
           !cloud_dl.done && (uint64_t)cloud_dl.readable < (uint64_t)jump_start + 65536)
    {
        monitor_mark(ACT_SD);
        download_watch();
        poll_events();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Reads the block at read_pos into block; its length, 0 at the end, -1 on
 * an error. */
static int read_block(void)
{
    if (read_pos >= size)
    {
        return 0;
    }
    if (read_pos >= end_pos)
    {
        debug_printf("pl: language chapter end %lu (7380)\n", (unsigned long)read_pos);
        return 0;
    }
    wait_language_chapter();
    if (stop_request)
    {
        return -1;
    }
    uint32_t n = size - read_pos < TAF_BLOCK ? size - read_pos : TAF_BLOCK;
    if (downloading && read_pos + n > cloud_dl.readable && !cloud_dl.done)
    {
        /* a block the download has not written completely
         * ends the playback; the download goes on. */
        debug_printf("pl: block %lu not loaded (%lu), end\n", (unsigned long)read_pos,
                     (unsigned long)cloud_dl.readable);
        dl_starved = true;
        return -1;
    }
    CRITICAL_ENTER();
    uint8_t *a = ahead_len == n && ahead_pos == read_pos ? ahead : NULL;
    reading = a != NULL;
    CRITICAL_EXIT();
    ahead_len = 0;
    if (a)
    {
        memcpy(block, a, n);
        reading = false;
        stats.ahead++;
        return (int)n;
    }
    UINT got;
    uint32_t t0 = tick_ms();
    FRESULT r = f_lseek(&file, read_pos);
    if (r == FR_OK)
    {
        r = f_read(&file, block, n, &got);
    }
    /* the longest card read, a wait behind a download's write included */
    if (tick_ms() - t0 > stats.read_max)
    {
        stats.read_max = tick_ms() - t0;
    }
    if (r == FR_DISK_ERR && !prompt_mode)
    {
        /* the data read failed also when repeated (
         * sd.c). */
        journal_write_content(uid_playing);
        panic_from(ACT_SD, PANIC_SD_CONTENT, read_pos, "content read failed");
    }
    if (r != FR_OK || got != n)
    {
        return -1;
    }
    return (int)n;
}

/* The next block into ahead, when it is on the card and no download
 * write runs; failing, read_block() reads it. */
static void read_ahead(void)
{
    want_card = false;
    if (!ahead_on || ahead_len || jump_start || read_pos >= size || read_pos >= end_pos)
    {
        return;
    }
    uint32_t n = size - read_pos < TAF_BLOCK ? size - read_pos : TAF_BLOCK;
    if (downloading && read_pos + n > cloud_dl.readable && !cloud_dl.done)
    {
        return;
    }
    CRITICAL_ENTER();
    uint8_t *a = ahead;
    reading = a && !writing;
    want_card = a && !reading;
    CRITICAL_EXIT();
    if (!reading)
    {
        return;
    }
    UINT got;
    if (f_lseek(&file, read_pos) == FR_OK && f_read(&file, a, n, &got) == FR_OK && got == n)
    {
        ahead_pos = read_pos;
        ahead_len = n;
    }
    reading = false;
}

void player_write_begin(void)
{
    /* a reading ahead the player waits for goes first, 100 ms at most */
    for (unsigned i = 0;; i++)
    {
        CRITICAL_ENTER();
        bool go = !reading && (!want_card || i >= 50);
        writing = go;
        CRITICAL_EXIT();
        if (go)
        {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void player_write_end(void)
{
    writing = false;
}

void player_ahead_begin(uint8_t *buf)
{
    ahead_len = 0;
    ahead = buf;
}

void player_ahead_end(void)
{
    CRITICAL_ENTER();
    ahead = NULL;
    CRITICAL_EXIT();
    while (reading)
    {
        vTaskDelay(1);
    }
    ahead_len = 0;
    want_card = false;
}

/* after block 1, the jump to J when it is usable. */
static void start_jump(void)
{
    uint32_t j = jump_start;
    jump_start = 0;
    bool ok = !(j % TAF_BLOCK) && j >= 2 * TAF_BLOCK && (uint64_t)j + TAF_BLOCK <= size;
    if (ok)
    {
        uint32_t keep = read_pos;
        bool starved = dl_starved;
        read_pos = j;
        int n = read_block();
        ok = n > 0 && taf_block_ok(block, (unsigned)n, j, hdr.audio_id);
        read_pos = keep;
        dl_starved = starved;
    }
    if (ok)
    {
        (void)jump_to(j, "start position (288)");
    }
    else
    {
        debug_printf("pl: start %lu bad, from 0 (287)\n", (unsigned long)j);
    }
}

/* chime 7 when the download of the placed figure's content
 * completes, for every audio_id (also for own content, whose audio_id
 * teddyCloud sets below 0x40000000). */
static void download_complete(void)
{
    if (dl_complete)
    {
        return;
    }
    dl_complete = true;
    debug_printf("pl: download complete, audio_id %08lx (1108)\n", (unsigned long)dl_audio_id);
    if (playing && !paused)
    {
        ledui_request(LED_READY_PLAYING);
    }
    sound_chime(CHIME_DOWNLOAD_COMPLETE);
}

/* a download that broke off is requested again from its
 * position after 1 s, then 2 s, 4 s and 8 s after each failed request,
 * while the playback goes on. */
static void download_watch(void)
{
    cloud_download_t *d = &cloud_dl;
    if (d->serial != dl_serial)
    {
        downloading = false; /* overtaken by another request */
        return;
    }
    if (d->done)
    {
        downloading = false;
        download_complete();
        return;
    }
    if (!d->failed)
    {
        return;
    }
    if (d->error == CE_RANGE)
    {
        /* the file goes to the trash after the playback, and
         * the figure is handled again. */
        debug_printf("pl: resume refused (416)\n");
        downloading = false;
        range_refused = true;
        STOP("416");
        return;
    }
    uint32_t now = tick_ms();
    if (!retry_at)
    {
        retry_wait = d->data ? 1000 : (retry_wait ? (retry_wait < 8000 ? retry_wait * 2 : 8000) : 1000);
        retry_at = now + retry_wait;
        debug_printf("pl: download broke at %lu, %s, retry %lu ms\n", (unsigned long)d->pos,
                     cloud_error_name(d->error), (unsigned long)retry_wait);
        return;
    }
    if ((int32_t)(now - retry_at) < 0)
    {
        return;
    }
    retry_at = 0;
    cloud_content(uid_playing, has_token_playing ? token_playing : NULL, d->pos, hdr.audio_id);
    dl_serial = cloud_dl.serial;
}

static void error_indication(int prompt);

/* after a block at x failed the check, the valid part of the
 * file is determined; unless it ends exactly at x, the file is marked
 * outdated. Not for a file the download still writes. */
static void invalid_block(uint32_t x)
{
    if (prompt_mode || downloading)
    {
        return;
    }
    uint32_t v = taf_resume_offset(&file, size, hdr.audio_id, block);
    debug_printf("pl: bad block %lu, ok to %lu (6468)\n", (unsigned long)x, (unsigned long)v);
    if (v == x)
    {
        debug_printf("pl: valid to bad block (6475)\n");
        return;
    }
    char path[32];
    sd_content_path(uid_playing, path);
    sd_set_outdated(path, true);
}

static bool content_read, content_ended;

static void play_content(void)
{
    dl_starved = false;
    content_read = content_ended = false;
    memset(&st, 0, sizeof st);
    memset(&stats, 0, sizeof stats);
    stop_request = false;
    stop_why = "?";
    out = NULL;
    read_pos = TAF_BLOCK;
    ahead_len = 0;
    ahead_on = true;
    started_ms = last_jump_ms = tick_ms();
    bool ended = false, error = false, started = false;
    uint32_t t0 = tick_ms();

    if (dec_too_small)
    {
        f_close(&file);
        playing = false;
        return;
    }
    while (!stop_request)
    {
        monitor_mark(ACT_SD);
        if (downloading)
        {
            download_watch();
        }
        poll_events(); /* a prompt queued meanwhile cuts in */
        if (stop_request)
        {
            break;
        }
        if (shuf.on && !jump_start && read_pos >= shuf.chap_end && !shuffle_next())
        {
            ended = true; /* as at the end of the content */
            break;
        }
        int n = read_block();
        if (stop_request)
        {
            break;
        }
        if (n == 0)
        {
            ended = true;
            break;
        }
        if (n < 0 || !taf_block_ok(block, (unsigned)n, read_pos, hdr.audio_id))
        {
            debug_printf("pl: block %lu %s\n", (unsigned long)read_pos,
                         n < 0 ? "unreadable" : "invalid");
            if (n > 0)
            {
                invalid_block(read_pos);
            }
            error = true;
            break;
        }
        uint32_t x = read_pos;
        read_pos += (uint32_t)n;
        if (!started && !prompt_mode)
        {
            log_event(EVT_PLAYBACK_STARTED, uid_playing); /* type 12 */
        }
        started = content_read = true;
        if (!prompt_mode)
        {
            int c = shuf.on ? shuf.order[shuf.index] : current_chapter();
            pub_chapter = c > 0 ? (uint32_t)c : 0;
        }
        /* not while the resume jump is pending. */
        if (!jump_start && !prompt_mode)
        {
            resume.valid = true;
            resume.tag_only = false;
            memcpy(resume.uid, uid_playing, 8);
            resume.block = read_pos / TAF_BLOCK;
        }
        st.ending = false;
        unsigned pos = 0;
        while (pos < (unsigned)n && !stop_request)
        {
            const uint8_t *pg = block + pos;
            unsigned segs = pg[26], len = 27 + segs;
            for (unsigned i = 0; i < segs; i++)
            {
                len += pg[27 + i];
            }
            /* before the resume jump, page 2 at the end of
             * block 1 is left out. */
            bool skip = jump_start && x == TAF_BLOCK && le32(pg + 18) == 2 && pos + len == (unsigned)n;
            if (!skip)
            {
                process_page(pg);
            }
            pos += len;
            poll_events();
        }
        if (st.ending)
        {
            st.dead = true;
        }
        if (jump_start && read_pos == 2 * TAF_BLOCK)
        {
            start_jump();
        }
    }

    if (stop_request || error)
    {
        audio_out_flush();
        out = NULL;
    }
    else
    {
        emit_end();
        bool idle = false;
        for (unsigned w = 0; !idle && w < 3000; w += 500)
        {
            monitor_mark(ACT_SD);
            idle = audio_out_wait_idle(500);
        }
        if (!idle)
        {
            debug_printf("pl: output busy 3 s\n");
        }
    }
    ahead_on = want_card = false;
    f_close(&file);
    playing = false;
    debug_printf("pl: %s at %lu, %lu ms, %lu pk, max %lu ms, und %lu, rd %lu ms, ra %lu\n",
                 stop_request ? stop_why : error ? "error"
                                                 : "end",
                 (unsigned long)read_pos,
                 (unsigned long)(tick_ms() - t0), (unsigned long)stats.packets, (unsigned long)stats.max_ms,
                 (unsigned long)audio_out_underruns(), (unsigned long)stats.read_max, (unsigned long)stats.ahead);
    content_ended = ended;
    if (ended && !prompt_mode)
    {
        resume.block = 0;
        if (hdr.audio_id > 0x3FFFFFFFu)
        {
            prompt_push(6, false);
        }
    }
    if (error && !prompt_mode)
    {
        error_indication(dl_starved ? 15 : -1);
    }
}

/* the error indication of a failed cloud operation the user
 * waits for: error pattern, then the sound prompt; -1 for none. */
static void error_indication(int prompt)
{
    debug_printf("pl: error, prompt %d\n", prompt);
    ledui_request(LED_ERROR);
    if (prompt >= 0)
    {
        prompt_push((unsigned)prompt, false);
    }
}

static int failure_prompt(cloud_error_t e, int status)
{
    if (net_offline())
    {
        return 10;
    }
    if (status == 410 || e == CE_REFUSED)
    {
        return 18;
    }
    switch (e)
    {
    case CE_NAME:
        return 21;
    case CE_CONNECT:
        return 23;
    case CE_UNAVAILABLE:
        return 22;
    default:
        break;
    }
    switch (net_state())
    {
    case 0:
        return 17;
    case 1:
    case 2:
        return 24;
    case 3:
        return 20;
    default:
        debug_printf("pl: no prompt, state %u\n", net_state());
        return -1;
    }
}

/* Moves the content file of uid to the trash once the download no longer
 * holds it open (at most 2 s); cancel: the download is stopped first. */
static void trash_content(const uint8_t uid[8], bool cancel)
{
    if (cancel)
    {
        cloud_content_cancel();
    }
    for (unsigned w = 0; cloud_download_file_open() && w < 2000; w += 10)
    {
        monitor_mark(ACT_SD);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    sd_trash(uid);
}

/* after a 416 the file goes to the trash and the placed figure,
 * if it is the one of the file, is handled as if just placed. */
static void range_not_satisfiable(const uint8_t uid[8])
{
    range_refused = false;
    debug_printf("pl: 416, file to trash\n");
    trash_content(uid, false);
    char path[32];
    FILINFO fi;
    sd_content_path(uid, path);
    if (f_stat(path, &fi) == FR_OK)
    {
        /* not moved: a new request would get the 416 again */
        debug_printf("pl: %s not moved\n", path);
        error_indication(-1);
        return;
    }
    figure_take();
    if (figure_present && !memcmp(figure_uid, uid, 8))
    {
        figure_changed = true;
    }
}

/* the language chapter of an audio_id 1 file as start
 * position J and end; false when the whole file plays. */
static bool language_chapter(void)
{
    unsigned l = settings_language();
    if (hdr.audio_id != 1 || hdr.chapters < 4 || l > 3)
    {
        return false;
    }
    uint64_t start = (uint64_t)hdr.chapter[l] * TAF_BLOCK;
    if (l + 1 < hdr.chapters)
    {
        end_pos = (uint64_t)hdr.chapter[l + 1] * TAF_BLOCK;
    }
    /* a start beyond 32 bits is beyond the file: J not usable (287) */
    jump_start = start < 0xFFFFFFFFull ? (uint32_t)start : 0xFFFFF000u;
    debug_verbose("pl: language chapter %u from %lu (7376)\n", l + 1, (unsigned long)jump_start);
    return true;
}

/* --- F1 and F2 state --- */

static ext_state_t xst; /* the figure's record as read at the start */
static bool xst_ok, x_resume, x_shuffle;

static bool profile_on(ext_key_t k)
{
    const ext_settings_t *p = xp_profile();
    return ext_has(p, k) && p->value[k] == 1;
}

/* Start position and shuffle order of a complete file; returns J. */
static uint32_t ext_start(const uint8_t uid[8], bool ref)
{
    uint32_t j = ref ? resume.block * TAF_BLOCK : 0;
    x_resume = profile_on(EXT_RESUME);
    x_shuffle = profile_on(EXT_SHUFFLE);
    xst_ok = (x_resume || x_shuffle) && ext_state_get(uid, &xst);
    bool same = xst_ok && xst.audio_id == hdr.audio_id;
    if (!ref && x_resume && same)
    {
        j = xst.position * TAF_BLOCK;
        debug_verbose("player: stored position, block %lu\n", (unsigned long)xst.position);
    }
    memset(&shuf, 0, sizeof shuf);
    if (!x_shuffle || downloading || hdr.audio_id == 1 || hdr.chapters < 2 || hdr.chapters > TAF_MAX_CHAPTERS)
    {
        return j;
    }
    shuf.on = true;
    shuf.n = (uint8_t)hdr.chapters;
    /* the same order when the figure resumes */
    if (same && (xst.flags & EXT_STATE_SHUFFLING) && xst.chapters == shuf.n && xst.index < shuf.n &&
        (x_resume || resume_names(uid)))
    {
        shuf.seed = xst.seed;
        shuf.index = xst.index;
        ext_shuffle(shuf.seed, shuf.n, shuf.order);
        debug_verbose("player: shuffle order continues at %u, chapter %u\n", shuf.index + 1,
                      shuf.order[shuf.index] + 1);
    }
    else
    {
        shuffle_new(xst_ok ? xst.seed : 0);
        j = 0;
    }
    shuf.chap_end = chap_end(shuf.order[shuf.index]);
    if (!j)
    {
        uint64_t st0 = chap_start(shuf.order[shuf.index]);
        j = st0 > TAF_BLOCK && st0 < size ? (uint32_t)st0 : 0;
    }
    return j;
}

/* the record after the playback, when something changed. */
static void ext_stop(const uint8_t uid[8])
{
    if (!(x_resume || x_shuffle) || !content_read)
    {
        return;
    }
    ext_state_t r = {0};
    memcpy(r.uid, uid, 8);
    r.audio_id = hdr.audio_id;
    r.position = content_ended ? 0 : resume_names(uid) ? resume.block
                                                       : 0;
    if (shuf.on)
    {
        r.seed = shuf.seed;
        r.index = shuf.index;
        r.chapters = shuf.n;
        r.flags = shuf.finished || content_ended ? 0 : EXT_STATE_SHUFFLING;
    }
    else if (xst_ok)
    {
        r.seed = xst.seed;
    }
    if (xst_ok && xst.audio_id == r.audio_id && xst.position == r.position && xst.seed == r.seed &&
        xst.index == r.index && xst.chapters == r.chapters && (xst.flags & 1) == r.flags)
    {
        return;
    }
    if (!ext_state_put(&r))
    {
        debug_printf("pl: state file not written\n");
    }
}

/* Plays the open file (state Content). */
static uint32_t content_t0; /* start_content() began: the "pl: play" line says how long it took */

/* records 6480 and 297 for the content file opened for
 * playback. */
static void __attribute__((noinline)) log_content_open(const uint8_t uid[8])
{
    char path[32];
    FILINFO fi;
    sd_content_path(uid, path);
    log_args_t a = {0};
    log_arg_text(&a, path);
    log_arg_num(&a, (int32_t)size);
    log_arg_num(&a, f_stat(path, &fi) == FR_OK ? (int32_t)sd_fat_time(fi.fdate, fi.ftime) : 0);
    log_fn(LOG_INFO, 22, 6480, 0, &a);
    a.n = 0;
    log_arg_num(&a, (int32_t)hdr.audio_id);
    log_arg_num(&a, hdr.chapters);
    log_fn(LOG_INFO, 22, 297, 0, &a);
}

static void play_file(const uint8_t uid[8])
{
    size = (uint32_t)f_size(&file);
    dl_audio_id = hdr.audio_id;
    debug_printf("pl: play %lu B, id %08lx, %u ch%s, after %lu ms\n", (unsigned long)size,
                 (unsigned long)hdr.audio_id, hdr.chapters, downloading ? ", downloading" : "",
                 (unsigned long)(tick_ms() - content_t0));
    log_content_open(uid);
    /* Resume. With a running download the box also resumes, when the
     * resume block is already on the card, as the original box does
     * (placed again during its download, the figure plays on where it
     * was removed); shuffle starts only once the content
     * is complete. */
    jump_start = 0;
    end_pos = size;
    bool ref = resume_names(uid) && resume.block;
    uint32_t resume_j = ext_start(uid, ref);
    if (downloading)
    {
        x_shuffle = false;
        if (resume_j && (uint64_t)resume_j + TAF_BLOCK > cloud_dl.readable && !cloud_dl.done)
        {
            debug_printf("pl: resume %lu not loaded (%lu)\n", (unsigned long)resume_j,
                         (unsigned long)cloud_dl.readable);
            resume_j = 0;
        }
    }
    if (language_chapter())
    {
        /* a resume position inside the language chapter is kept */
        if (resume_j > jump_start && resume_j < end_pos)
        {
            jump_start = resume_j;
        }
    }
    else
    {
        jump_start = resume_j;
    }
    /* content download until it completes, then ready */
    ledui_request(downloading && !dl_complete ? LED_CONTENT_DOWNLOAD : LED_READY_PLAYING);
    preparing = false;
    playing = true;
    play_content();
    downloading = false;
    if (!stopped_all)
    {
        ledui_request(LED_READY_IDLE); /* after stop_all the net activity owns the LED */
    }
    ext_stop(uid);
    shuf.on = false;
    if (range_refused)
    {
        range_not_satisfiable(uid);
    }
}

/* waits until the download has written 65536 bytes or is
 * complete; false when it failed first or the figure was removed (the
 * download then goes on). */
static bool wait_download(bool *removed)
{
    *removed = false;
    for (;;)
    {
        monitor_mark(ACT_SD);
        cloud_download_t *d = &cloud_dl;
        if (d->serial != dl_serial || d->failed)
        {
            return false;
        }
        if (d->done)
        {
            download_complete();
        }
        if (d->done || d->readable >= 65536)
        {
            return true;
        }
        event_t e;
        if (xQueueReceive(queue, &e, pdMS_TO_TICKS(20)) == pdTRUE && !ext_event(&e))
        {
            debug_printf("pl: %s while preparing (6218)\n", gesture_name(&e));
        }
        if (power_off_wanted)
        {
            return false;
        }
        sync_request_busy();
        figure_take();
        if (stop_all_req)
        {
            stop_all_req = false;
            stopped_all = *removed = true;
            figure_changed = figure_present; /* handled again after setup mode */
            return false;
        }
        if (figure_changed && !figures_held())
        {
            /* held back during an online round */
            figure_changed = false;
            if (figure_removed || !figure_present)
            {
                figure_removed = false;
                figure_changed = figure_present;
                *removed = true;
                log_event(EVT_PLAYBACK_STOPPED, NULL);
                xp_remove();
                debug_printf("pl: removed while loading\n");
                return false;
            }
            debug_printf("pl: placed while preparing (6157)\n");
        }
    }
}

static void start_content(const uint8_t uid[8], const uint8_t *token)
{
    content_t0 = tick_ms();
    char path[32];
    sd_content_path(uid, path);
    memcpy(uid_playing, uid, 8);
    has_token_playing = token != NULL;
    if (token)
    {
        memcpy(token_playing, token, 32);
    }
    downloading = false;
    retry_at = retry_wait = 0;
    prompt_mode = false;
    range_refused = false;
    dl_complete = false;
    dl_audio_id = 0;

    taf_state_t s = taf_open(&file, path, &hdr, block, false);
    static const char *const names[] = {"complete", "incomplete", "missing", "outdated", "invalid", "not readable"};
    debug_printf("pl: %s %s\n", path, names[s]);
    uint32_t offset = 0, if_range = 0;
    bool fallback = false;
    if ((s == TAF_COMPLETE || s == TAF_OUTDATED) && cloud_content_running(uid))
    {
        /* The download creates the file at its full size and marks it until
         * it completes: with Z a multiple of 4096 it looks complete or
         * outdated before the last byte has arrived. Follow the download. */
        if (s == TAF_COMPLETE)
        {
            f_close(&file);
        }
        s = TAF_INCOMPLETE;
        dl_audio_id = hdr.audio_id;
    }
    else
    {
        switch (s)
        {
        case TAF_COMPLETE:
            if (hdr.audio_id != 1)
            {
                cloud_claim(uid, token);
                play_file(uid);
                return;
            }
            dl_audio_id = 1;
            f_close(&file);
            fallback = true;
            break;
        case TAF_OUTDATED:
            fallback = true;
            break;
        case TAF_INCOMPLETE:
            dl_audio_id = hdr.audio_id;
            offset = taf_resume_offset(&file, (uint32_t)f_size(&file), hdr.audio_id, block);
            if (offset)
            {
                if_range = hdr.audio_id;
            }
            f_close(&file);
            break;
        case TAF_INVALID:
            trash_content(uid, true); /* a download may still write it */
            break;
        case TAF_MISSING:
            break;
        case TAF_READ_ERROR:
            error_indication(-1);
            return;
        }
    }

    ledui_request(LED_CONTENT_DOWNLOAD);
    if (s == TAF_INCOMPLETE && cloud_content_running(uid))
    {
        /* The download goes on after a removal and is only cancelled by
         * a request for another figure or a stall: follow
         * it. A new request would also wait for the network processor to
         * release the TLS session of the dropped one. */
        debug_printf("pl: download still running at %lu, following it\n", (unsigned long)cloud_dl.pos);
    }
    else
    {
        debug_verbose("player: requesting the content from offset %lu\n", (unsigned long)offset);
        cloud_content(uid, token, offset, if_range);
    }
    dl_serial = cloud_dl.serial;
    bool removed;
    if (!wait_download(&removed))
    {
        if (removed || power_off_wanted)
        {
            if (!stopped_all && !power_off_wanted)
            {
                ledui_request(LED_READY_IDLE);
            }
            return;
        }
        cloud_download_t *d = &cloud_dl;
        if (d->status == 410)
        {
            trash_content(uid, false);
        }
        else if (d->error == CE_RANGE && d->serial == dl_serial)
        {
            ledui_request(LED_READY_IDLE);
            range_not_satisfiable(uid);
            return;
        }
        else if (fallback && !d->data)
        {
            /* the cached file, if complete. */
            if (taf_open(&file, path, &hdr, block, true) == TAF_COMPLETE)
            {
                debug_printf("pl: download FAILED %s, cached\n", cloud_error_name(d->error));
                play_file(uid);
                return;
            }
        }
        ledui_request(LED_READY_IDLE);
        error_indication(failure_prompt(d->error, d->status));
        return;
    }
    s = taf_open(&file, path, &hdr, block, true);
    if (s != TAF_COMPLETE && s != TAF_INCOMPLETE)
    {
        /* a broken header from the cloud. */
        debug_printf("pl: downloaded file %s\n", names[s]);
        trash_content(uid, true);
        ledui_request(LED_READY_IDLE);
        error_indication(-1);
        return;
    }
    /* A completion after the wait is seen by download_watch (chime 7). */
    downloading = !dl_complete;
    play_file(uid);
}

/* --- sound prompts --- */

static void prompt_path(unsigned set, unsigned nn, char *path)
{
    uint8_t uid[8] = {0, 0, 0, (uint8_t)set, 0, 0, 0, (uint8_t)nn};
    sd_content_path(uid, path);
}

static bool prompt_open(unsigned set, unsigned nn)
{
    char path[32];
    prompt_path(set, nn, path);
    taf_state_t s = taf_open(&file, path, &hdr, block, true);
    if (s == TAF_INCOMPLETE)
    {
        f_close(&file);
    }
    return s == TAF_COMPLETE;
}

static void play_prompt(unsigned nn, bool wait)
{
    for (unsigned w = 0; wait && w < 1000; w += 500)
    {
        monitor_mark(ACT_SD);
        vTaskDelay(500);
    }
    unsigned l = settings_language();
    bool ok = prompt_open(l, nn);
    if (!ok && l != 0)
    {
        ok = prompt_open(0, nn);
    }
    if (!ok)
    {
        sfx_unpack(block);
        ok = prompt_open(0, nn);
    }
    if (!ok)
    {
        debug_printf("pl: prompt %u, no file (1693)\n", nn);
        return;
    }
    debug_printf("pl: sound prompt %u\n", nn);
    stop_prompt_req = false; /* a stop for an earlier prompt */
    prompt_mode = true;
    downloading = false;
    jump_start = 0;
    size = (uint32_t)f_size(&file);
    end_pos = size;
    playing = false;
    play_content();
    prompt_mode = false;
}

/* --- sync request and its result --- */

static void sync_act(void)
{
    if (net_offline())
    {
        error_indication(10);
        return;
    }
    uint8_t s = net_state();
    if (s >= 4)
    {
        ledui_request(LED_SYNC);
        cloud_sync_request(true);
        return;
    }
    error_indication(s == 0 ? 17 : s == 3 ? 20
                                          : 24);
}

static void sync_result(int error, bool user)
{
    ledui_request(LED_READY_IDLE);
    if (!user)
    {
        return; /* no indication for an automatic sync */
    }
    if (!error)
    {
        prompt_push(1, false);
    }
    else
    {
        error_indication(failure_prompt((cloud_error_t)error, 0));
    }
}

/* Command 71: the content of a UID as for a placed figure with
 * 32 zero data bytes, with its profile, until it ends, a stop or a
 * placement. */
static void remote_play(void)
{
    static const uint8_t zero[32];
    uint8_t u[8];
    CRITICAL_ENTER();
    memcpy(u, rc_uid, 8);
    CRITICAL_EXIT();
    debug_verbose("pl: remote play\n");
    ledui_request(LED_ACTIVITY);
    log_event(EVT_PLAYBACK_STARTING, u);
    if (!xp_place(u))
    {
        remote_playing = preparing = true;
        set_busy(true);
        start_content(u, zero);
        set_busy(false);
        remote_playing = preparing = false;
    }
    xp_remove();
}

/* --- remote control API, log activity --- */

int player_remote_pause(void)
{
    if (!playing || paused)
    {
        return PLAYER_RC_STATE;
    }
    rc_pause = true;
    wake();
    return PLAYER_RC_DONE;
}

int player_remote_continue(void)
{
    if (!paused)
    {
        return PLAYER_RC_STATE;
    }
    rc_continue = true;
    wake();
    return PLAYER_RC_DONE;
}

void player_switch_off(void)
{
    if (!xp_ears_off() || power_shutting_down())
    {
        debug_printf("pl: F9 %s\n", power_shutting_down() ? "shutting down" : "disabled");
        return;
    }
    debug_printf("pl: F9 switch-off\n");
    if (playing)
    {
        ledui_request(LED_READY_IDLE); /* remembered: the stop does not end the blink */
    }
    ledui_request(LED_EXT_LILAC);
    switch_off = true;
    wake();
    power_off_soon();
}

int player_remote_stop(void)
{
    if (!playing)
    {
        return PLAYER_RC_STATE;
    }
    rc_stop = true;
    wake();
    return PLAYER_RC_DONE;
}

int player_remote_chapter(bool forward)
{
    if (!playing || downloading)
    {
        return PLAYER_RC_STATE;
    }
    player_chapter(forward);
    return PLAYER_RC_DONE;
}

int player_remote_volume(unsigned step)
{
    if (step > 15)
    {
        return PLAYER_RC_ARGS;
    }
    unsigned path = sound_path();
    if (path != 1 && path != 2)
    {
        return PLAYER_RC_STATE;
    }
    sound_set_step(0, step);
    return step > sound_limit(path) ? PLAYER_RC_STATE : PLAYER_RC_DONE;
}

int player_remote_play(const uint8_t uid[8])
{
    CRITICAL_ENTER();
    bool ok = !fig_in.present && !busy && !rc_play;
    if (ok)
    {
        memcpy(rc_uid, uid, 8);
        rc_play = true;
    }
    CRITICAL_EXIT();
    if (!ok || cloud_sync_running())
    {
        return rc_play = false, PLAYER_RC_STATE;
    }
    wake();
    return PLAYER_RC_DONE;
}

void player_status(player_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->state = paused ? 3 : playing      ? 2
                        : preparing      ? 4
                        : fig_in.present ? 1
                                         : 0;
    CRITICAL_ENTER();
    if (s->state >= 2)
    {
        memcpy(s->uid, uid_playing, 8);
    }
    else if (s->state == 1)
    {
        memcpy(s->uid, fig_in.uid, 8);
    }
    CRITICAL_EXIT();
    if (s->state == 2 || s->state == 3)
    {
        s->audio_id = hdr.audio_id;
        s->chapter = pub_chapter;
        s->position = read_pos / TAF_BLOCK;
    }
    unsigned path = sound_path();
    s->step = (uint8_t)sound_step(path);
    s->limit = (uint8_t)sound_limit(path);
    s->headphones = path == 2;
    for (int i = 0; i < 3; i++)
    {
        if (player_lock((player_lock_t)i, NULL) == 1)
        {
            s->locks |= (uint8_t)(1u << i);
        }
    }
}

/* --- power-off (step 3) --- */

bool player_idle(void)
{
    return !busy && !prompt_pending() && !cloud_sync_running();
}

/* In the player activity: playback has stopped (positions stored as on
 * removal); pending requests are dropped, then the housekeeping. */
static void power_off_work(void)
{
    CRITICAL_ENTER();
    prompt_tail = prompt_head;
    CRITICAL_EXIT();
    sync_wanted = sync_req_in = false;
    figure_changed = false;
    xp_power_off();
    FATFS *fs;
    DWORD free_n;
    if (!cloud_dl.active && f_getfree("", &free_n, &fs) == FR_OK)
    {
        uint32_t n = fs->n_fatent - 2;
        debug_verbose("pl: housekeeping\n");
        sd_room(n * 10 / 100, n * 30 / 100, NULL, block);
    }
    power_off_done = true;
    for (;;)
    {
        monitor_mark(ACT_SD);
        vTaskDelay(pdMS_TO_TICKS(500)); /* nothing more until the box is off */
    }
}

void player_power_off(void)
{
    power_off_wanted = true;
    wake();
    while (!power_off_done)
    {
        monitor_feed();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void task(void *arg)
{
    (void)arg;
    if (opus_decoder_get_size(2) > (int)sizeof dec_mem)
    {
        debug_printf("pl: decoder %d B, no play\n", opus_decoder_get_size(2));
        dec_too_small = true;
    }
    xp_start();
    bool held_logged = false;
    for (;;)
    {
        monitor_mark(ACT_SD);
        if (power_off_wanted)
        {
            power_off_work();
            continue;
        }
        stopped_all = false;
        if (stop_all_req)
        {
            stop_all_req = false; /* nothing playing: idle already */
        }
        rc_pause = rc_continue = rc_stop = false; /* nothing to act on */
        if (rc_play)
        {
            rc_play = false;
            figure_take();
            if (!figure_present && !prompt_pending() && !figures_held() && !startup_hold)
            {
                remote_play();
            }
            continue;
        }
        if (xp_poll())
        {
            /* the timer ran out while no content plays */
            debug_printf("pl: sleep timer up\n");
            power_off_soon();
        }
        figure_take();
        if (prompt_pending() && !cloud_sync_running())
        { /* prompts wait for a sync */
            CRITICAL_ENTER();
            unsigned i = prompt_tail % sizeof prompt_fifo;
            unsigned nn = prompt_fifo[i];
            bool wait = prompt_wait[i];
            prompt_tail++;
            CRITICAL_EXIT();
            set_busy(true);
            /* A prompt waits for the configured codec (up to 10 s). */
            for (unsigned w = 0; !audio_out_running() && w < 100; w++)
            {
                monitor_mark(ACT_SD);
                vTaskDelay(100);
            }
            if (audio_out_running())
            {
                play_prompt(nn, wait);
            }
            else
            {
                debug_printf("pl: prompt %u, no output\n", nn);
            }
            set_busy(false);
            if (nn == 0)
            {
                startup_hold = false;
            }
            if (prompt_cut)
            {
                prompt_cut = false;
                figure_changed = true; /* the figure counts as newly placed */
            }
            continue;
        }
        if (figure_changed && figures_held())
        {
            /* A placement ends a running session and is acted
             * on once the session or the online round has ended. */
            cloud_sync_abort();
            if (!held_logged)
            {
                debug_verbose("pl: figure held\n");
            }
            held_logged = true;
            event_t e;
            if (xQueueReceive(queue, &e, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                ext_event(&e); /* gestures: no effect when idle */
            }
            continue;
        }
        held_logged = false;
        if (figure_changed && !startup_hold && power_critical_pending())
        {
            /* the power-off comes first. */
            event_t e;
            if (xQueueReceive(queue, &e, pdMS_TO_TICKS(1000)) == pdTRUE)
            {
                ext_event(&e);
            }
            continue;
        }
        if (figure_changed && !startup_hold)
        {
            /* act once on the figure state now. */
            figure_changed = figure_removed = false;
            if (figure_present)
            {
                cloud_sync_abort();
                ledui_request(LED_ACTIVITY);
                log_event(EVT_PLAYBACK_STARTING, figure_uid);
                if (xp_place(figure_uid))
                {
                    continue; /* an event tag: the box stays idle */
                }
                set_busy(true);
                preparing = true;
                start_content(figure_uid, figure_has_token ? figure_token : NULL);
                preparing = false;
                set_busy(false);
            }
            else
            {
                xp_remove();
                debug_printf("pl: tag removed, idle (6158)\n");
            }
            continue;
        }
        if (result_in)
        {
            CRITICAL_ENTER();
            int error = result_error;
            bool user = result_user;
            result_in = false;
            CRITICAL_EXIT();
            sync_result(error, user);
            continue;
        }
        if (sync_req_in)
        {
            sync_req_in = false;
            sound_chime(CHIME_SIGNAL); /* acknowledge tone */
            sync_wanted = true;
            continue;
        }
        if (sync_wanted)
        {
            sync_wanted = false;
            sync_act();
            continue;
        }
        event_t e;
        /* Idle: chapter and wind have no effect; the progress mark at least
         * once per second. */
        if (xQueueReceive(queue, &e, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            ext_event(&e);
        }
    }
}

void player_init(void)
{
    queue = xQueueCreateStatic(QUEUE_LEN, sizeof(event_t), queue_buf, &queue_mem);
}

void player_start(void)
{
    static StackType_t stack[TASK_STACK(5120)];
    static StaticTask_t tcb;
    /* Above the cloud and log activities: decoding must keep up while TLS
     * connections are made. */
    xTaskCreateStatic(task, "player", TASK_STACK(5120), NULL, 3, stack, &tcb);
}
