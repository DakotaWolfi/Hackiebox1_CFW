/* Host harness: player.c with stubs, simulated time = audio output. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "FreeRTOS.h"
#include "ff.h"
#include "simplelink.h"
#include "audio_out.h"
#include "cloud.h"
#include "player.h"
#include "rtnl.h"
#include "sound.h"
#include "ledui.h"
#include "settings.h"
#include "clock.h"
#include "monitor.h"
#include "journal.h"
#include "power.h"

static const char *root;
static uint32_t sim_ms;
static jmp_buf done;
static void (*task_fn)(void *);
static FILE *pcm_out;
static uint64_t frames_out;

/* --- RTOS --- */
typedef struct hq
{
    uint8_t buf[64][64];
    unsigned size, head, tail;
} hq_t;
static hq_t qs[4];
static int nq;
QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t size, uint8_t *b, StaticQueue_t *q)
{
    (void)len;
    (void)b;
    (void)q;
    qs[nq].size = size;
    return &qs[nq++];
}
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t t)
{
    (void)t;
    if (q->head - q->tail >= 64)
    {
        return pdFALSE;
    }
    memcpy(q->buf[q->head++ % 64], it, q->size);
    return pdTRUE;
}
static void script_idle(void);
static void script_tick(void);
static bool script_pending(void);
BaseType_t xQueueReceive(QueueHandle_t q, void *it, TickType_t t)
{
    if (q->head == q->tail)
    {
        if (getenv("IDLETIME") && script_pending())
        {
            /* env IDLETIME: waits pass in simulated time */
            uint32_t d = t == portMAX_DELAY || t >= 1000 ? 1000 : t;
            sim_ms += d;
            frames_out += 16ull * d;
            script_tick();
        }
        else if (t == portMAX_DELAY || t >= 1000)
        {
            script_idle(); /* idle wait */
        }
        else
        {
            sim_ms += t;
        }
        if (q->head == q->tail)
        {
            return pdFALSE;
        }
    }
    memcpy(it, q->buf[q->tail++ % 64], q->size);
    return pdTRUE;
}
void vTaskDelay(TickType_t t) { sim_ms += t; }
TaskHandle_t xTaskCreateStatic(void (*f)(void *), const char *n, uint32_t d, void *p, UBaseType_t pr, StackType_t *s, StaticTask_t *tc)
{
    (void)n;
    (void)d;
    (void)p;
    (void)pr;
    (void)s;
    (void)tc;
    task_fn = f;
    return NULL;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 0;
}
uint32_t tick_ms(void) { return sim_ms; }
void tick_wait(uint32_t n) { sim_ms += n; }
void monitor_mark(activity_t a) { (void)a; }
void monitor_box_activity(bool b) { printf("[%6u] STUB box activity %s\n", sim_ms, b ? "busy" : "idle"); }
void journal_write_content(const uint8_t u[8])
{
    (void)u;
    printf("[%6u] STUB content journal\n", sim_ms);
}
void panic_from(activity_t a, panic_id_t id, uint32_t v, const char *m)
{
    printf("[%6u] STUB panic %d %d %u %s\n", sim_ms, a, id, v, m);
    exit(1);
}
void debug_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    printf("[%6u] ", sim_ms);
    vprintf(f, a);
    va_end(a);
}

/* --- audio --- */
static int16_t abuf[2 * AUDIO_OUT_FRAMES];
int16_t *audio_out_buffer(void) { return abuf; }
static void script_tick(void);
static void dl_tick(void);
static void full(const char *p, char *out);
void audio_out_commit(void)
{
    if (pcm_out)
    {
        fwrite(abuf, 4, AUDIO_OUT_FRAMES, pcm_out);
    }
    frames_out += AUDIO_OUT_FRAMES;
    sim_ms += 16;
    dl_tick();
    script_tick();
}
void audio_out_commit_frames(unsigned n)
{
    if (pcm_out)
    {
        fwrite(abuf, 4, n, pcm_out);
    }
    frames_out += n;
    sim_ms += n / 16;
    printf("[%6u] STUB last buffer %u frames\n", sim_ms, n);
    script_tick();
}
void audio_out_end(void) {}
void audio_out_flush(void) {}
bool audio_out_wait_idle(uint32_t ms)
{
    (void)ms;
    return true;
}
bool audio_out_running(void) { return true; }
void audio_out_report(void) {}
uint32_t audio_out_underruns(void) { return 0; }

/* --- other modules --- */
cloud_download_t cloud_dl;
/* env CLOUD: unset = every request fails; "range" = a ranged request gets
 * 416; "dl:<full file>" = the file is copied onto the card and becomes
 * readable DLRATE (default 8192) bytes per output buffer. env HOLD=<from>:<to>: figures held
 * in that span of simulated ms. */
static uint32_t dl_size;
static uint8_t lent[4096]; /* the cloud activity's memory lent for the reading ahead */
static void dl_tick(void)
{
    if (!dl_size || cloud_dl.done || cloud_dl.failed)
    {
        return;
    }
    cloud_dl.readable += getenv("DLRATE") ? (uint32_t)atoi(getenv("DLRATE")) : 8192;
    cloud_dl.pos = cloud_dl.readable;
    if (getenv("DLFAIL") && cloud_dl.readable >= (uint32_t)atoi(getenv("DLFAIL")))
    {
        cloud_dl.failed = true;
        cloud_dl.error = CE_TRANSFER;
        player_ahead_end();
        printf("[%6u] STUB download breaks off\n", sim_ms);
        return;
    }
    if (cloud_dl.readable >= dl_size)
    {
        cloud_dl.readable = cloud_dl.pos = dl_size;
        cloud_dl.done = true;
        cloud_dl.active = false;
        player_ahead_end();
        printf("[%6u] STUB download done\n", sim_ms);
    }
}
void cloud_content(const uint8_t uid[8], const uint8_t *t, uint32_t o, uint32_t r)
{
    const char *m = getenv("CLOUD");
    (void)uid;
    (void)t;
    cloud_dl.serial++;
    cloud_dl.failed = cloud_dl.done = cloud_dl.data = false;
    cloud_dl.error = CE_OK;
    cloud_dl.status = 0;
    if (((m && !strcmp(m, "range")) || (getenv("RANGE416") && dl_size)) && o)
    {
        printf("[%6u] STUB cloud_content offset %u if-range %u -> 416\n", sim_ms, o, r);
        cloud_dl.failed = true;
        cloud_dl.error = CE_RANGE;
        cloud_dl.status = 416;
        return;
    }
    if (m && !strncmp(m, "dl:", 3))
    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "cp %s %s/CONTENT/B706EE0E/500304E0", m + 3, root);
        if (system(cmd))
        {
            abort();
        }
        struct stat st;
        stat(m + 3, &st);
        dl_size = (uint32_t)st.st_size;
        printf("[%6u] STUB cloud_content offset %u if-range %u -> downloading %u bytes\n", sim_ms, o, r, dl_size);
        cloud_dl.active = cloud_dl.data = true;
        cloud_dl.readable = cloud_dl.pos = o > 65536 ? o : 65536;
        cloud_dl.size = dl_size;
        player_ahead_begin(lent);
        return;
    }
    printf("[%6u] STUB cloud_content offset %u if-range %u -> fails\n", sim_ms, o, r);
    cloud_dl.failed = true;
    cloud_dl.error = CE_CONNECT;
}
void cloud_content_cancel(void) {}
bool cloud_content_running(const uint8_t uid[8])
{
    (void)uid;
    return cloud_dl.active && !cloud_dl.failed;
}
bool cloud_download_file_open(void) { return false; }
void cloud_claim(const uint8_t uid[8], const uint8_t *t)
{
    (void)uid;
    (void)t;
    printf("[%6u] STUB claim\n", sim_ms);
}
const char *cloud_error_name(cloud_error_t e) { return e ? "error" : "ok"; }
bool cloud_holds_figures(void)
{
    const char *h = getenv("HOLD");
    char *e;
    if (!h)
    {
        return false;
    }
    unsigned long a = strtoul(h, &e, 10), b = strtoul(e + 1, NULL, 10);
    return sim_ms >= a && sim_ms < b;
}
bool cloud_sync_running(void) { return false; }
/* --- extension stubs: sound with steps per path, settings, clock, log --- */
settings_t settings;
static unsigned snd_path = 1, snd_step[3] = {0, 8, 8}, snd_lim[3] = {15, 15, 15};
static uint32_t snd_ears;
void sound_ext_limits(uint8_t a, uint8_t b)
{
    snd_lim[1] = a <= 15 ? a : 15;
    snd_lim[2] = b <= 15 ? b : 15;
    for (int p = 1; p <= 2; p++)
    {
        if (snd_step[p] > snd_lim[p])
        {
            snd_step[p] = snd_lim[p];
        }
    }
    printf("[%6u] SOUND limits %u %u, steps %u %u\n", sim_ms, a, b, snd_step[1], snd_step[2]);
}
void sound_set_step(unsigned p, unsigned st)
{
    if (!p)
    {
        p = snd_path;
    }
    snd_step[p] = st > snd_lim[p] ? snd_lim[p] : st;
    printf("[%6u] SOUND path %u step %u\n", sim_ms, p, snd_step[p]);
}
void sound_fade(unsigned s16)
{
    snd_step[snd_path] = s16 / 16;
    if (getenv("FADELOG"))
    {
        printf("[%6u] SOUND fade %u/16\n", sim_ms, s16);
    }
}
void sound_feedback(uint16_t r, unsigned n) { printf("[%6u] SOUND feedback %u pairs, results %04x\n", sim_ms, n, r); }
unsigned sound_path(void) { return snd_path; }
unsigned sound_step(unsigned p) { return snd_step[p]; }
unsigned sound_limit(unsigned p) { return snd_lim[p]; }
uint32_t sound_ear_steps(void) { return snd_ears; }
static void ear_up(void)
{
    if (snd_step[snd_path] < snd_lim[snd_path])
    {
        snd_step[snd_path]++;
    }
    snd_ears++;
    printf("[%6u] SOUND ear: step %u\n", sim_ms, snd_step[snd_path]);
}
void ledui_ext_level(int l) { printf("[%6u] LED level %d\n", sim_ms, l); }
uint32_t clock_now(void) { return 1791775993u; }
void log_arg_num(log_args_t *a, int32_t v)
{
    if (a->n + 4 <= sizeof a->data)
    {
        memcpy(a->data + a->n, &v, 4);
        a->n += 4;
    }
}
void log_arg_text(log_args_t *a, const char *t)
{
    size_t n = strlen(t) + 1;
    if (a->n + n <= sizeof a->data)
    {
        memcpy(a->data + a->n, t, n);
        a->n += (uint8_t)n;
    }
}
uint32_t sd_fat_time(uint16_t d, uint16_t t)
{
    (void)t;
    return d ? 1791776107u : 0;
}
void log_fn(uint8_t l, uint16_t g, uint16_t f, uint8_t v, const log_args_t *a)
{
    (void)v;
    printf("[%6u] RECORD level %u group %u function %u, %u bytes\n", sim_ms, l, g, f, a->n);
}
void monitor_feed(void) {}
uint32_t pwrstate_resume(const uint8_t u[8], uint32_t b) { return (u[0] | (u[1] & 15u) << 8) | (b / 2 > 16383 ? 16383 : b / 2) << 12; }
bool sd_room(uint32_t t, uint32_t g, volatile bool *s, uint8_t *w)
{
    (void)s;
    (void)w;
    printf("[%6u] STUB room %u %u\n", sim_ms, t, g);
    return true;
}
void cloud_sync_abort(void) {}
void cloud_sync_request(bool u) { (void)u; }
void ledui_request(led_request_t r) { (void)r; }
void log_event(log_event_t t, const uint8_t *u)
{
    (void)u;
    printf("[%6u] EVENT %d\n", sim_ms, t);
}
void sound_chime(chime_t c) { printf("[%6u] CHIME %d\n", sim_ms, c); }
uint8_t net_state(void) { return 6; }
void power_off_soon(void) { printf("[%6u] STUB power-off requested\n", sim_ms); }
void power_idle_minutes(uint16_t battery, uint16_t station)
{
    static uint16_t last[2];
    if (battery != last[0] || station != last[1])
    {
        printf("[%6u] STUB idle power-off minutes %u/%u\n", sim_ms, battery, station);
        last[0] = battery;
        last[1] = station;
    }
}
static bool critical;
bool power_critical_pending(void) { return critical; }
bool power_shutting_down(void) { return false; }
bool net_offline(void) { return false; }
bool net_setup_active(void) { return false; }
uint8_t settings_language(void) { return 0; }
void sd_content_path(const uint8_t uid[8], char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    strcpy(path, "CONTENT/00000000/00000000");
    for (int i = 0; i < 8; i++)
    {
        char *d = path + 8 + 2 * i + (i >= 4);
        d[0] = hex[uid[i] >> 4];
        d[1] = hex[uid[i] & 15];
    }
}
bool sd_make_dirs(const char *p)
{
    char q[512], *c;
    full(p, q);
    for (c = q + strlen(root) + 1; (c = strchr(c, '/')); c++)
    {
        *c = 0;
        mkdir(q, 0777);
        *c = '/';
    }
    return true;
}
void sd_move_aside(const uint8_t uid[8], const char *dir)
{
    (void)dir;
    char p[32], q[512];
    sd_content_path(uid, p);
    full(p, q);
    remove(q);
    printf("[%6u] STUB trash\n", sim_ms);
}
void sd_set_outdated(const char *p, bool on) { printf("[%6u] STUB outdated %s %d\n", sim_ms, p, on); }
/* No flash files: no prompt archive. */
long flashfs_size(const char *name)
{
    (void)name;
    return -14;
}
long flashfs_read(const char *name, uint32_t offset, void *buf, unsigned n)
{
    (void)name;
    (void)offset;
    (void)buf;
    (void)n;
    return -14;
}

/* --- FatFs over the host --- */
static void full(const char *p, char *out) { snprintf(out, 512, "%s/%s", root, p[0] == '/' ? p + 1 : p); }
FRESULT f_open(FIL *f, const char *p, BYTE m)
{
    char q[512];
    full(p, q);
    f->fp = fopen(q, (m & FA_CREATE_ALWAYS) ? "w+b" : (m & FA_WRITE) ? "r+b"
                                                                     : "rb");
    if (!f->fp)
    {
        return FR_NO_FILE;
    }
    fseek(f->fp, 0, SEEK_END);
    f->size = (FSIZE_t)ftell(f->fp);
    rewind(f->fp);
    return FR_OK;
}
FRESULT f_read(FIL *f, void *b, UINT n, UINT *g)
{
    *g = (UINT)fread(b, 1, n, f->fp);
    return FR_OK;
}
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *w)
{
    *w = (UINT)fwrite(b, 1, n, f->fp);
    return FR_OK;
}
FRESULT f_getfree(const char *p, DWORD *n, FATFS **fs)
{
    static FATFS f = {1002, 64};
    (void)p;
    *n = 500;
    *fs = &f;
    return FR_OK;
}
FRESULT f_lseek(FIL *f, FSIZE_t o) { return fseek(f->fp, (long)o, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
FRESULT f_close(FIL *f)
{
    if (f->fp)
    {
        fclose(f->fp);
    }
    f->fp = NULL;
    return FR_OK;
}
FRESULT f_stat(const char *p, FILINFO *fi)
{
    char q[512];
    struct stat st;
    full(p, q);
    if (stat(q, &st))
    {
        return FR_NO_FILE;
    }
    memset(fi, 0, sizeof *fi);
    fi->fsize = (FSIZE_t)st.st_size;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    return FR_OK;
}
FRESULT f_unlink(const char *p)
{
    (void)p;
    return FR_OK;
}

/* --- script --- */
typedef struct
{
    uint32_t at_audio_ms;
    char what;
} step_t;
static step_t *script;
static unsigned script_n, script_i;
static const uint8_t uid[8] = {0xB7, 0x06, 0xEE, 0x0E, 0x50, 0x03, 0x04, 0xE0};
static const uint8_t uid_event[8] = {0x11, 0x22, 0x33, 0x44, 0x50, 0x03, 0x04, 0xE0};
static const uint8_t uid_other[8] = {0xC1, 0xC2, 0xC3, 0xC4, 0x50, 0x03, 0x04, 0xE0};
static const uint8_t token[32];
static void act(char w)
{
    switch (w)
    {
    case 'P':
        printf("--- place\n");
        player_placed(uid, token);
        break;
    case 'R':
        printf("--- remove\n");
        player_removed();
        break;
    case 'F':
        printf("--- chapter forward\n");
        player_chapter(true);
        break;
    case 'B':
        printf("--- chapter back\n");
        player_chapter(false);
        break;
    case 'W':
        printf("--- wind forward\n");
        player_wind(true);
        break;
    case 'V':
        printf("--- wind back\n");
        player_wind(false);
        break;
    case 'X':
        printf("--- prompt 6\n");
        player_prompt(6, false);
        break;
    case 'C':
        printf("--- critical battery: prompt 9, power-off pending\n");
        critical = true;
        player_prompt(9, false);
        break;
    case 'S':
        printf("--- start-up prompt\n");
        player_prompt(0, false);
        break;
    case 'p':
        printf("--- remote pause -> %d\n", player_remote_pause());
        break;
    case 'c':
        printf("--- remote continue -> %d\n", player_remote_continue());
        break;
    case 's':
        printf("--- remote stop -> %d\n", player_remote_stop());
        break;
    case 'n':
        printf("--- remote chapter forward -> %d\n", player_remote_chapter(true));
        break;
    case 'v':
        printf("--- remote volume 12 -> %d\n", player_remote_volume(12));
        break;
    case 'r':
        printf("--- remote play -> %d\n", player_remote_play(uid_other));
        break;
    case 'i':
    {
        player_status_t st;
        player_status(&st);
        printf("--- status: state %u uid %02X%02X.. audio_id %08x chapter %u position %u step %u limit %u hp %u locks %u\n", st.state, st.uid[0], st.uid[1], st.audio_id, st.chapter, st.position, st.step, st.limit, st.headphones, st.locks);
        break;
    }
    case 'K':
        printf("--- place other figure\n");
        player_placed(uid_other, token);
        break;
    case 'Q':
        printf("--- place event tag\n");
        player_placed(uid_event, token);
        break;
    case 'L':
        printf("--- ear (LED level)\n");
        player_ear_level(true);
        break;
    case 'H':
        printf("--- one ear held (ears switch)\n");
        player_ears_switch();
        break;
    case 'U':
        printf("--- big ear (volume)\n");
        ear_up();
        break;
    case 'T':
        printf("--- slap upside down\n");
        player_restart_gesture(sim_ms);
        break;
    case 'J':
        printf("--- headphones\n");
        snd_path = 3 - snd_path;
        break;
    case 'O':
        printf("--- both ears, short (F9)\n");
        player_switch_off();
        break;
    case 'G':
    {
        uint8_t k[2] = {(uint8_t)snd_step[1], (uint8_t)snd_step[2]};
        player_timer_steps(k);
        printf("--- steps %u %u, kept at a power-off %u %u\n", snd_step[1], snd_step[2], k[0], k[1]);
        break;
    }
    }
}
static bool script_pending(void) { return script_i < script_n && script[script_i].at_audio_ms; }
static void script_tick(void)
{
    uint32_t audio = (uint32_t)(frames_out / 16);
    while (script_i < script_n && script[script_i].at_audio_ms && script[script_i].at_audio_ms <= audio)
    {
        act(script[script_i++].what);
    }
}
static void script_idle(void)
{
    /* Idle: steps with time 0 are taken when the player waits. */
    if (script_i < script_n && script[script_i].at_audio_ms == 0)
    {
        act(script[script_i++].what);
        return;
    }
    if (script_i < script_n)
    {
        frames_out = (uint64_t)script[script_i].at_audio_ms * 16;
        act(script[script_i++].what);
        return;
    }
    longjmp(done, 1);
}

int main(int argc, char **argv)
{
    root = argv[1];
    if (argc > 3)
    {
        pcm_out = fopen(argv[3], "wb");
    }
    /* script: comma list of <audio ms><letter>, e.g. 0P,5000F,30000R,0P */
    script = calloc(64, sizeof *script);
    for (char *t = strtok(argv[2], ","); t; t = strtok(NULL, ","))
    {
        script[script_n].at_audio_ms = (uint32_t)strtoul(t, &t, 10);
        script[script_n++].what = *t;
    }
    player_init();
    player_start();
    if (!setjmp(done))
    {
        task_fn(NULL);
    }
    printf("=== done: %llu frames output (%llu ms)\n", (unsigned long long)frames_out, (unsigned long long)frames_out / 16);
    return 0;
}
