/*
 * App slots, trial start, rollback and the update of item 3 on the ESP32
 * box: the platform side of bootstate.h,
 * update.h, handover.h and buildblock.h, which the CC3200 implements with
 * the SD bootloader. The image has no build block: its build number is the
 * build date and time of the app description.
 *
 * State in the NVS namespace cfw, blob "slots": the original
 * slot, the slot last confirmed, the slot of a pending trial and the slot
 * it was installed from, the unconfirmed starts, the last result and the
 * rolled-back build (for the hello). When the NVS cannot be
 * used, the unconfirmed starts are counted in RTC memory. A start that ends
 * before the count in the NVS (a crash in the NVS start) is counted in RTC
 * memory before anything else; an image confirmed through the update
 * interface is not rolled back by that count.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bootstate.h"
#include "buildblock.h"
#include "cloud.h"
#include "debug.h"
#include "esp32.h"
#include "ext.h"
#include "handover.h"
#include "net.h"
#include "port.h"
#include "sha256.h"
#include "sound.h"
#include "tick.h"
#include "update.h"
#include "version.h"

#define PROJECT_CFW "toniebox-cfw-esp32"
#define PROJECT_OFW "toniebox-esp32-eu"
#define NONE 0xFF
#define SLOT_SIZE 0x220000u
#define DIGEST_TEXT 64u

typedef struct
{
    uint8_t version;
    uint8_t original;  /* 0..2, 3 = none, NONE = not yet looked for */
    uint8_t confirmed; /* slot last confirmed, NONE */
    uint8_t trial;     /* slot of a pending trial, NONE */
    uint8_t from;      /* the slot that trial was installed from */
    uint8_t tries;     /* unconfirmed starts of the trial */
    uint8_t last_result;
    uint8_t reserved;
    uint32_t rolled_back;
} slots_t;

static slots_t st;
static uint8_t running = NONE;
static bool trial_pending, confirmed;
static uint8_t checks_ok, checks_failed;
static handover_t handover;
static RTC_NOINIT_ATTR uint32_t rtc_tries[2]; /* slot | magic, count */
static RTC_NOINIT_ATTR uint32_t rtc_early[2]; /* slot | magic, starts not past slots_start */

static const esp_partition_t *slot_part(unsigned n)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + n),
                                    NULL);
}

static void save(void)
{
    if (nvsfile_write("slots", &st, sizeof st) != (long)sizeof st)
    {
        debug_printf("slots: state not stored\n");
    }
}

/* "v5.226.0" or "CFW_0.1.0" as a number for comparing versions. */
static uint32_t version_number(const char *v)
{
    uint32_t r = 0;
    int parts = 0;
    while (*v && parts < 3)
    {
        if (*v >= '0' && *v <= '9')
        {
            r = r * 1000u + (uint32_t)strtoul(v, (char **)&v, 10);
            parts++;
        }
        else
        {
            v++;
        }
    }
    while (parts++ < 3)
    {
        r *= 1000u;
    }
    return r;
}

static bool describe(unsigned n, esp_app_desc_t *d)
{
    const esp_partition_t *p = slot_part(n);
    return p && esp_ota_get_partition_description(p, d) == ESP_OK;
}

/* the slot with the original firmware and the highest
 * version; 3 when there is none. */
static uint8_t find_original(void)
{
    uint8_t best = 3;
    uint32_t best_v = 0;
    for (unsigned n = 0; n < 3; n++)
    {
        esp_app_desc_t d;
        if (describe(n, &d) && !strcmp(d.project_name, PROJECT_OFW))
        {
            uint32_t v = version_number(d.version);
            if (best == 3 || v > best_v)
            {
                best = (uint8_t)n;
                best_v = v;
            }
        }
    }
    return best;
}

static void __attribute__((noreturn)) roll_back(const char *why)
{
    uint8_t to = st.from;
    debug_printf("slots: rollback to slot %u (%s)\n", to, why);
    st.trial = NONE;
    st.tries = 0;
    st.last_result = 2;
    st.rolled_back = CFW_BUILD_TIME;
    save();
    rtc_tries[0] = 0;
    const esp_partition_t *p = to < 3 ? slot_part(to) : NULL;
    if (!p || esp_ota_set_boot_partition(p) != ESP_OK)
    {
        debug_printf("slots: rollback impossible, restart\n");
    }
    port_restart();
}

void slots_early(void)
{
    const esp_partition_t *r = esp_ota_get_running_partition();
    if (!r || r->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 || r->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_2)
    {
        return;
    }
    uint8_t n = (uint8_t)(r->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
    if (rtc_early[0] == (0xEA510000u | n))
    {
        rtc_early[1]++;
    }
    else
    {
        rtc_early[0] = 0xEA510000u | n;
        rtc_early[1] = 1;
    }
    if (rtc_early[1] < 3)
    {
        return;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(r, &state) == ESP_OK && state == ESP_OTA_IMG_VALID)
    {
        return; /* confirmed: no trial */
    }
    /* The slot it was installed from is in the namespace, which is what
     * fails: back to the original firmware, which can install again. */
    uint8_t to = find_original();
    debug_printf("slots: start %lu of slot %u ended early, back to slot %u\n", (unsigned long)rtc_early[1], n, to);
    rtc_early[0] = 0;
    const esp_partition_t *p = to < 3 && to != n ? slot_part(to) : NULL;
    if (p && esp_ota_set_boot_partition(p) == ESP_OK)
    {
        port_restart();
    }
    debug_printf("slots: no slot to go back to\n");
}

void slots_start(void)
{
    const esp_partition_t *r = esp_ota_get_running_partition();
    running = r && r->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && r->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_2
                  ? (uint8_t)(r->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0)
                  : NONE;
    if (nvsfile_read("slots", 0, &st, sizeof st) != (long)sizeof st || st.version != 1)
    {
        memset(&st, NONE, sizeof st);
        st.version = 1;
        st.tries = 0;
        st.last_result = 0;
        st.rolled_back = 0;
    }
    if (st.original == NONE)
    {
        st.original = find_original(); /* first start */
    }
    if (running == NONE || (st.confirmed == running && st.trial != running))
    {
        trial_pending = false;
    }
    else
    {
        trial_pending = true;
        if (st.trial != running)
        {
            /* Installed by the original firmware or found
             * without a record: back to the original firmware. */
            st.trial = running;
            st.from = st.confirmed != NONE && st.confirmed != running ? st.confirmed : st.original;
            st.tries = 0;
        }
        st.tries++;
        /* The count also in RTC memory, for an NVS that cannot be written. */
        if (rtc_tries[0] == (0xC0FE0000u | running))
        {
            rtc_tries[1]++;
        }
        else
        {
            rtc_tries[0] = 0xC0FE0000u | running;
            rtc_tries[1] = 1;
        }
        save();
        debug_printf("slots: trial start %u of slot %u, from %u\n", st.tries, running, st.from);
        /* at the third unconfirmed start, before anything else. */
        if (st.tries >= 3 || rtc_tries[1] >= 3)
        {
            roll_back("third unconfirmed start");
        }
    }
    rtc_early[0] = 0; /* from here on the count above holds */
    handover.valid = true;
    handover.slot = running;
    handover.kind = trial_pending ? HANDOVER_TRIAL : HANDOVER_ACTIVE;
    handover.magic = true;
    debug_printf("slots: running %u, original %u, confirmed %u%s\n", running, st.original, st.confirmed,
                 trial_pending ? ", trial" : "");
}

unsigned slots_running(void)
{
    return running;
}

unsigned slots_original(void)
{
    return st.original;
}

/* the User-Agent of the original ESP firmware,
 * project name and version of the original slot; the captured one when
 * there is none. */
const char *update_original_agent(void)
{
    static char agent[sizeof(((esp_app_desc_t *)0)->project_name) + sizeof(((esp_app_desc_t *)0)->version) + 1];
    static uint8_t from = NONE;
    esp_app_desc_t d;
    if (!agent[0] || from != st.original)
    {
        from = st.original;
        if (from < 3 && describe(from, &d) && d.version[0])
        {
            snprintf(agent, sizeof agent, "%.32s/%.32s", d.project_name, d.version);
        }
        else
        {
            snprintf(agent, sizeof agent, "%s/v5.233.0", PROJECT_OFW);
        }
    }
    return agent;
}

/* --- handover.h, buildblock.h --- */

void handover_capture(void)
{
}

const handover_t *handover_get(void)
{
    return &handover;
}

uint8_t buildblock_slot(void)
{
    return running == NONE ? 7 : running;
}

/* the build number of an image, its build date and time in the
 * app description ("Mmm dd yyyy", "hh:mm:ss") read as UTC, in seconds since
 * 1970; 0 when the fields are empty or have another form. */
static uint32_t desc_build(const esp_app_desc_t *d)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = d->date, *time = d->time;
    int month = -1;
    for (int m = 0; m < 12; m++)
    {
        if (!strncmp(date, months + 3 * m, 3))
        {
            month = m;
        }
    }
    int n[6];
    const char *at[6] = {date + 4, date + 7, date + 9, time, time + 3, time + 6};
    for (int i = 0; i < 6; i++)
    {
        char hi = at[i][0], lo = at[i][1];
        if (i == 0 && hi == ' ')
        {
            hi = '0';
        }
        if (hi < '0' || hi > '9' || lo < '0' || lo > '9')
        {
            return 0;
        }
        n[i] = (hi - '0') * 10 + (lo - '0');
    }
    if (month < 0 || date[3] != ' ' || date[6] != ' ' || date[11] || time[2] != ':' || time[5] != ':' || time[8] ||
        !n[0] || n[3] > 23 || n[4] > 59 || n[5] > 59)
    {
        return 0;
    }
    /* days since 1970-01-01 of the civil date, March-based year */
    int y = n[1] * 100 + n[2] - (month < 2);
    int era = y / 400, yoe = y - era * 400, mp = (month + 10) % 12;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + (153 * mp + 2) / 5 + n[0] - 1;
    long days = (long)era * 146097 + doe - 719468;
    return days < 0 ? 0 : (uint32_t)(days * 86400 + n[3] * 3600 + n[4] * 60 + n[5]);
}

/* the app description must say the build number the code was
 * built with (tools/esp32img.py stamp writes it after the link). */
void buildblock_check(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    uint32_t b = desc_build(d);
    debug_printf("boot: %s %s build %lu, slot %u\n", d->project_name, d->version, (unsigned long)b, running);
    if (b != CFW_BUILD_TIME)
    {
        debug_printf("boot: app description %s %s is not build %lu (image not stamped)\n", d->date, d->time,
                     (unsigned long)CFW_BUILD_TIME);
    }
}

/* --- bootstate.h --- */

bool bootstate_read(uint8_t s[32])
{
    memset(s, 0, 32);
    s[4] = st.confirmed;
    s[5] = st.trial;
    s[6] = trial_pending ? 1 : 0;
    s[7] = st.tries;
    s[9] = st.last_result;
    s[10] = NONE;
    memcpy(s + 12, &st.rolled_back, 4);
    return true;
}

bool bootstate_trial(uint8_t slot)
{
    st.trial = slot;
    st.from = running;
    st.tries = 0;
    st.last_result = 0;
    save();
    return true;
}

bool bootstate_confirm(void)
{
    if (!trial_pending)
    {
        return false;
    }
    st.confirmed = running;
    st.trial = NONE;
    st.tries = 0;
    st.last_result = 1;
    save();
    rtc_tries[0] = 0;
    esp_ota_mark_app_valid_cancel_rollback();
    trial_pending = false;
    confirmed = true;
    handover.kind = HANDOVER_ACTIVE;
    ext_trial_confirmed(); /* the hello answer reports it */
    debug_printf("slots: slot %u confirmed\n", running);
    return true;
}

bool bootstate_trial_pending(void)
{
    return trial_pending;
}

void bootstate_report(boot_check_t c, bool ok)
{
    if (ok)
    {
        checks_ok |= 1u << c;
    }
    else
    {
        checks_failed |= 1u << c;
    }
}

void bootstate_trial_poll(bool joined)
{
    static bool ever_joined;
    const uint8_t hw = 1u << BOOT_CHECK_CARD | 1u << BOOT_CHECK_CODEC | 1u << BOOT_CHECK_READER;
    if (!trial_pending)
    {
        return;
    }
    uint32_t now = tick_ms();
    ever_joined |= joined;
    if (checks_failed & hw)
    {
        roll_back("hardware check failed");
    }
    /* without a server configuration the join counts as the
     * time request. */
    bool time_ok = (checks_ok & 1u << BOOT_CHECK_TIME) || (ever_joined && !cloud_configured());
    if ((checks_ok & hw) == hw && (time_ok || (now >= 60000u && !ever_joined)))
    {
        bootstate_confirm();
        return;
    }
    if (joined && now >= 300000u)
    {
        roll_back("no time after 300 s");
    }
}

/* --- update.h: item 3 into an app slot --- */

typedef enum
{
    U_IDLE,
    U_HEADER,  /* collecting the image's first bytes */
    U_WRITING, /* into an app slot */
} u_state_t;

static struct
{
    u_state_t state;
    uint32_t length, image_len, pos;
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    char digest[DIGEST_TEXT];
    sha256_t sha;
    esp_ota_handle_t ota;
    const esp_partition_t *target;
    bool ofw;
} u;
static bool restart_pending;

/* neither running nor original; 1 + slot, 0 when none. */
unsigned update_free_slot(void)
{
    for (unsigned n = 0; n < 3; n++)
    {
        if (n != running && n != st.original)
        {
            return n + 1;
        }
    }
    return 0;
}

bool update_begin(uint32_t length)
{
    update_abort();
    if (length < sizeof u.head + DIGEST_TEXT || length > SLOT_SIZE + DIGEST_TEXT)
    {
        debug_printf("update: %lu B: abandoned\n", (unsigned long)length);
        return false;
    }
    u.state = U_HEADER;
    u.length = length;
    u.image_len = length - DIGEST_TEXT;
    u.pos = 0;
    sha256_init(&u.sha);
    debug_printf("update: item 3, %lu bytes\n", (unsigned long)length);
    return true;
}

/* The first bytes: image header, first segment header, app description
 * (ESP-IDF documentation, App Image Format); chooses the slot and
 * refuses a foreign image or the running build. */
static bool choose(void)
{
    const esp_image_header_t *h = (const esp_image_header_t *)u.head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(u.head + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != ESP_CHIP_ID_ESP32S3 || d->magic_word != ESP_APP_DESC_MAGIC_WORD)
    {
        debug_printf("update: not an ESP32-S3 app image: abandoned\n");
        return false;
    }
    if (!strncmp(d->project_name, PROJECT_CFW, sizeof d->project_name))
    {
        /* by the build number of the app
         * description. */
        uint32_t b = desc_build(d);
        if (!b)
        {
            debug_printf("update: no build date and time: refused\n");
            return false;
        }
        if (b == CFW_BUILD_TIME)
        {
            debug_printf("update: the running build: refused\n");
            return false;
        }
        unsigned n = update_free_slot();
        u.target = n ? slot_part(n - 1) : NULL;
        u.ofw = false;
        debug_printf("update: %s %s into slot %u\n", d->project_name, d->version, n - 1);
    }
    else if (!strncmp(d->project_name, PROJECT_OFW, sizeof d->project_name))
    {
        /* back to the original firmware, as the server delivers
         * it (with its patches), into the free slot; the original slot
         * stays as it is. */
        u.ofw = true;
        unsigned n = update_free_slot();
        u.target = n ? slot_part(n - 1) : NULL;
        debug_printf("update: original firmware %s into slot %u\n", d->version, n - 1);
    }
    else
    {
        debug_printf("update: project \"%.32s\": refused\n", d->project_name);
        return false;
    }
    if (!u.target || esp_ota_begin(u.target, u.image_len, &u.ota) != ESP_OK)
    {
        debug_printf("update: slot not writable: abandoned\n");
        return false;
    }
    u.state = U_WRITING;
    return esp_ota_write(u.ota, u.head, sizeof u.head) == ESP_OK;
}

bool update_write(const uint8_t *p, size_t n)
{
    while (n && u.state != U_IDLE)
    {
        if (u.pos >= u.length)
        {
            return true; /* beyond the announced length: ignored */
        }
        if (u.pos >= u.image_len)
        {
            /* the digest text */
            size_t k = u.length - u.pos < n ? u.length - u.pos : n;
            memcpy(u.digest + (u.pos - u.image_len), p, k);
            u.pos += k;
            p += k;
            n -= k;
            continue;
        }
        size_t k = u.image_len - u.pos < n ? u.image_len - u.pos : n;
        if (u.state == U_HEADER)
        {
            size_t want = sizeof u.head - u.pos;
            k = k < want ? k : want;
            memcpy(u.head + u.pos, p, k);
            sha256_update(&u.sha, p, k);
            u.pos += k;
            p += k;
            n -= k;
            if (u.pos == sizeof u.head && !choose())
            {
                update_abort();
                return false;
            }
            continue;
        }
        sha256_update(&u.sha, p, k);
        if (u.state == U_WRITING && esp_ota_write(u.ota, p, k) != ESP_OK)
        {
            debug_printf("update: write at %lu failed: abandoned\n", (unsigned long)u.pos);
            update_abort();
            return false;
        }
        u.pos += k;
        p += k;
        n -= k;
    }
    return u.state != U_IDLE;
}

update_result_t update_finish(void)
{
    if (u.state == U_IDLE || u.state == U_HEADER || u.pos < u.length)
    {
        update_abort();
        return UPDATE_ABANDONED;
    }
    uint8_t d[32];
    char hex[DIGEST_TEXT + 1];
    sha256_final(&u.sha, d);
    for (int i = 0; i < 32; i++)
    {
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    }
    if (memcmp(hex, u.digest, DIGEST_TEXT))
    {
        debug_printf("update: digest differs: abandoned\n");
        update_abort();
        return UPDATE_ABANDONED;
    }
    if (u.state == U_WRITING)
    {
        esp_err_t e = esp_ota_end(u.ota); /* checks the image and its own digest */
        u.state = U_IDLE;
        if (e != ESP_OK)
        {
            debug_printf("update: image check %d: abandoned\n", e);
            return UPDATE_ABANDONED;
        }
    }
    u.state = U_IDLE;
    if (esp_ota_set_boot_partition(u.target) != ESP_OK)
    {
        debug_printf("update: boot selection not written: abandoned\n");
        return UPDATE_ABANDONED;
    }
    unsigned slot = u.target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    if (u.ofw)
    {
        /* The original firmware needs no confirmation of ours. It is the
         * original from now on; a later installation by it
         * returns to it, not to this firmware's slot. */
        st.trial = NONE;
        st.original = (uint8_t)slot;
        st.confirmed = NONE;
        save();
    }
    else
    {
        bootstate_trial((uint8_t)slot);
    }
    restart_pending = true;
    debug_printf("update: slot %u selected, restart follows\n", slot);
    return UPDATE_INSTALLED;
}

void update_abort(void)
{
    if (u.state == U_WRITING)
    {
        esp_ota_abort(u.ota);
    }
    u.state = U_IDLE;
}

bool update_restart_pending(void)
{
    return restart_pending;
}

void update_restart(void)
{
    sound_park(500);
    net_shutdown();
    debug_printf("update: restart (reset)\n");
    vTaskDelay(pdMS_TO_TICKS(100) + 1);
    port_restart();
}
