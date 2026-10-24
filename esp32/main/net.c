/*
 * Wi-Fi of the ESP32 box (the CC3200 box's behaviour): the
 * connection states and sessions, setup search, setup mode with the access
 * point and the setup page, the join attempt, the profile storage, offline
 * mode and the network shutdown, as src/cc3200/net.c does them with the
 * CC3200's network processor. The data rules are shared (setup.c).
 *
 * What the network processor does by itself is done here: the seven
 * profiles are kept in the NVS namespace cfw (blob "wifi"); the station
 * connects to the stored network with the best signal found by a scan,
 * again after a disconnection; setup mode has its own DHCP server (lwIP)
 * and answers every DNS question with the access point's address, so that
 * http://toniebox.start/ reaches the setup page.
 *
 * At the first start, without profiles of its own, the firmware takes the
 * profiles of the original firmware. The clock comes from a
 * time server by SNTP, not from the API host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "clock.h"
#include "cloud.h"
#include "debug.h"
#include "esp32.h"
#include "ledui.h"
#include "monitor.h"
#include "net.h"
#include "panic.h"
#include "player.h"
#include "port.h"
#include "rtnl.h"
#include "settings.h"
#include "setup.h"
#include "sound.h"
#include "tick.h"
#include "version.h"

void player_stop_all(void) __attribute__((weak));
void player_stop_prompt(void) __attribute__((weak));
void ledui_offline(bool on) __attribute__((weak));

typedef enum
{
    EV_DISCONNECT,
    EV_CONNECT,
    EV_ADDRESS,
    EV_SUBMIT,
    EV_SHUTDOWN,
    EV_SETUP,
    EV_RESCAN,
    EV_CLOUD_RESULT,
} ev_t;

typedef struct
{
    uint8_t kind;
    uint8_t len; /* SSID length, disconnection reason, setup language */
    uint8_t ssid[32];
    uint32_t ip, gateway;
    uint32_t info;
} event_t;

typedef enum
{
    M_STATION,
    M_SEARCH,
    M_SETUP,
    M_JOIN
} net_mode_t;

/* A stored profile. */
typedef struct
{
    uint8_t present;
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t key_len;
    uint8_t key[64];
    uint8_t sec; /* 0 open, 1 WEP, 2 WPA/WPA2 (setup_profile_security) */
} profile_t;

static StaticQueue_t queue_mem;
static uint8_t queue_buf[8 * sizeof(event_t)];
static QueueHandle_t queue;
static TaskHandle_t net_task;
static event_t latest;
static volatile bool latest_set;

static volatile uint8_t state;
static volatile uint32_t session, session_count;
static volatile bool going_down, handing_over, running;
static uint8_t mac[6];
static StaticSemaphore_t done_mem;
static SemaphoreHandle_t done;
static esp_netif_t *sta_if, *ap_if;

static volatile uint8_t mode;
static volatile bool offline, offline_req;
static volatile bool join_waiting, join_conn, join_addr, join_handover;
static setup_creds_t creds;
static volatile unsigned scan_n;
static scan_entry_t scan[SETUP_SCAN_MAX];
static volatile bool page_allowed, page_active;
static volatile bool profile_pending;
static profile_t pending;
static uint32_t ready_at;
static profile_t profiles[7];
static bool connecting;
static uint32_t next_try;
static int dns_sock = -1;

uint8_t net_state(void)
{
    return state;
}

uint32_t net_session(void)
{
    return session;
}

uint32_t net_sp_version(void)
{
    return 0;
}

/* the clock from the time server, not from the API host. */
static void on_time(struct timeval *tv)
{
    uint32_t t = (uint32_t)tv->tv_sec;
    if (!clock_set(t))
    {
        debug_printf("net: time %lu <= uptime\n", (unsigned long)t);
        return;
    }
    civil_t cv;
    clock_to_civil(t, &cv);
    debug_printf("net: clock %lu (%04lu-%02lu-%02lu %02lu:%02lu:%02lu)\n", (unsigned long)t, (unsigned long)cv.year,
                 (unsigned long)cv.month, (unsigned long)cv.day, (unsigned long)cv.hour, (unsigned long)cv.minute,
                 (unsigned long)cv.second);
}

void net_time_sync(const char *server)
{
    static char name[66];
    if (name[0])
    {
        return;
    }
    snprintf(name, sizeof name, "%s", server);
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(name);
    c.sync_cb = on_time;
    esp_err_t r = esp_netif_sntp_init(&c);
    debug_printf("net: time server %s%s\n", name, r == ESP_OK ? "" : ", not started");
}

bool net_handing_over(void)
{
    return handing_over || mode == M_SEARCH || join_handover;
}

bool net_offline(void)
{
    return offline;
}

void net_offline_restore(bool on)
{
    offline = on;
}

void net_offline_set(bool on)
{
    offline = on;
}

bool net_setup_active(void)
{
    return mode == M_SEARCH || mode == M_SETUP;
}

static void set_state(uint8_t s, const char *why)
{
    CRITICAL_ENTER();
    uint8_t was = state;
    state = s;
    CRITICAL_EXIT();
    if (s != was)
    {
        debug_printf("net: state %u -> %u (%s)\n", was, s, why);
    }
}

static void end_session(void)
{
    session = 0;
}

void net_cloud_state(uint32_t s, uint8_t to)
{
    static const uint8_t allowed[][2] = {{0, 4}, {3, 4}, {4, 5}, {4, 6}, {5, 4}, {5, 6}, {6, 4}, {6, 5}};
    CRITICAL_ENTER();
    uint8_t was = state;
    bool ok = s && s == session;
    bool step = was == to;
    for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
    {
        if (allowed[i][0] == was && allowed[i][1] == to)
        {
            step = true;
        }
    }
    if (ok && step)
    {
        state = to;
    }
    CRITICAL_EXIT();
    if (ok && step && was != to)
    {
        debug_printf("net: state %u -> %u (cloud)\n", was, to);
    }
}

/* --- events (from the system's event task) --- */

static void post_net(const event_t *e)
{
    CRITICAL_ENTER();
    latest = *e;
    latest_set = true;
    CRITICAL_EXIT();
    if (net_task)
    {
        xTaskNotifyGive(net_task);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    event_t ev = {0};
    if (!running)
    {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        const wifi_event_sta_connected_t *c = data;
        ev.kind = EV_CONNECT;
        ev.len = c->ssid_len <= 32 ? c->ssid_len : 0;
        memcpy(ev.ssid, c->ssid, ev.len);
        if (join_waiting)
        {
            join_conn = true;
        }
        post_net(&ev);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *d = data;
        ev.kind = EV_DISCONNECT;
        ev.len = d->reason;
        post_net(&ev);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *g = data;
        ev.kind = EV_ADDRESS;
        ev.ip = lwip_ntohl(g->ip_info.ip.addr);
        ev.gateway = lwip_ntohl(g->ip_info.gw.addr);
        if (join_waiting && ev.ip != ev.gateway)
        {
            join_addr = true;
        }
        post_net(&ev);
    }
}

static void post_request(uint8_t kind, uint32_t ip, uint32_t info)
{
    event_t e = {0};
    e.kind = kind;
    e.ip = ip;
    e.info = info;
    if (!queue)
    {
        return;
    }
    while (xQueueSend(queue, &e, pdMS_TO_TICKS(100)) != pdTRUE)
        ;
    if (net_task)
    {
        xTaskNotifyGive(net_task);
    }
}

/* --- setup page --- */

void net_setup_field(const uint8_t *name, unsigned name_len, const uint8_t *value, unsigned value_len)
{
    CRITICAL_ENTER();
    setup_post_t r = setup_post(&creds, name, name_len, value, value_len);
    bool submit = r == POST_SUBMIT && !join_waiting;
    bool ok = submit && setup_form(&creds);
    uint8_t lang = creds.language;
    CRITICAL_EXIT();
    if (ok)
    {
        event_t e = {0};
        e.kind = EV_SUBMIT;
        e.len = lang;
        post_net(&e);
    }
}

unsigned net_scan_list(const scan_entry_t **list)
{
    *list = scan;
    return mode == M_SETUP ? scan_n : 0;
}

bool net_page_begin(void)
{
    CRITICAL_ENTER();
    bool ok = mode == M_SETUP && page_allowed && !page_active;
    if (ok)
    {
        page_active = true;
    }
    CRITICAL_EXIT();
    return ok;
}

bool net_page_on(void)
{
    return page_allowed;
}

void net_page_end(bool took)
{
    (void)took;
    page_active = false;
}

static void page_stop(void)
{
    page_allowed = false;
    for (unsigned i = 0; page_active && i < 1000; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* --- profiles --- */

static void profiles_load(void)
{
    if (nvsfile_read("wifi", 0, profiles, sizeof profiles) != (long)sizeof profiles)
    {
        memset(profiles, 0, sizeof profiles);
    }
}

static void profiles_save(void)
{
    if (nvsfile_write("wifi", profiles, sizeof profiles) != (long)sizeof profiles)
    {
        debug_printf("net: profiles not stored\n");
    }
}

static unsigned profile_count(void)
{
    unsigned n = 0;
    for (int i = 0; i < 7; i++)
    {
        n += profiles[i].present ? 1 : 0;
    }
    return n;
}

/* the profiles of the original firmware, once, when there are
 * none of its own. Its namespace is only read. Each blob is the name or
 * passphrase without a terminating zero byte; every present profile is
 * taken, whatever INDEX holds. */
void net_import_original_profiles(void)
{
    if (profile_count())
    {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("TB_WIFI", NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }
    unsigned n = 0;
    for (int i = 0; i < 7; i++)
    {
        char key[8];
        uint8_t ssid[33], pw[65];
        size_t sl = sizeof ssid, pl = sizeof pw;
        snprintf(key, sizeof key, "SSID%d", i);
        if (nvs_get_blob(h, key, ssid, &sl) != ESP_OK)
        {
            continue;
        }
        snprintf(key, sizeof key, "PW%d", i);
        if (nvs_get_blob(h, key, pw, &pl) != ESP_OK)
        {
            pl = 0;
        }
        if (!sl || sl > 32 || pl > 64)
        {
            continue;
        }
        profile_t *p = &profiles[i];
        p->present = 1;
        p->ssid_len = (uint8_t)sl;
        memcpy(p->ssid, ssid, sl);
        p->key_len = (uint8_t)pl;
        memcpy(p->key, pw, pl);
        p->sec = pl ? 2 : 0;
        n++;
    }
    nvs_close(h);
    if (n)
    {
        profiles_save();
    }
    debug_printf("net: %u profiles of the original firmware taken\n", n);
}

long net_profiles_delete(void)
{
    memset(profiles, 0, sizeof profiles);
    long r = nvsfile_write("wifi", profiles, sizeof profiles) == (long)sizeof profiles ? 0 : -1;
    debug_printf("net: all profiles deleted: %ld\n", r);
    return r;
}

/* Stores the joined network as a profile, with the shared slot plan. */
static void store_profile(void)
{
    slot_info_t s[7];
    for (int i = 0; i < 7; i++)
    {
        memset(&s[i], 0, sizeof s[i]);
        s[i].present = profiles[i].present;
        s[i].len = profiles[i].ssid_len;
        memcpy(s[i].ssid, profiles[i].ssid, sizeof s[i].ssid);
        s[i].sec = profiles[i].sec;
    }
    slot_plan_t p;
    setup_slot_plan(s, pending.ssid, pending.ssid_len, pending.sec, &p);
    for (unsigned i = 0; i < p.n_before; i++)
    {
        profiles[p.del_before[i]].present = 0;
    }
    int slot = -1;
    for (int i = 0; i < 7 && slot < 0; i++)
    {
        if (!profiles[i].present)
        {
            slot = i;
        }
    }
    if (slot < 0)
    {
        slot = 6;
    }
    profiles[slot] = pending;
    profiles[slot].present = 1;
    log_args_t a = {0};
    log_arg_num(&a, slot);
    log_fn(LOG_INFO, 24, p.match ? 7184 : 7172, 0, &a);
    if (p.del_after >= 0 && slot != p.del_after)
    {
        profiles[p.del_after].present = 0;
        a.n = 0;
        log_arg_num(&a, p.del_after);
        log_fn(LOG_INFO, 24, 7178, 0, &a);
    }
    profiles_save();
    debug_printf("net: profile stored, slot %d, deleted %d\n", slot, p.del_after);
}

/* --- station: connecting to the best stored network --- */

static wifi_auth_mode_t threshold(uint8_t sec)
{
    return sec == 0 ? WIFI_AUTH_OPEN : sec == 1 ? WIFI_AUTH_WEP
                                                : WIFI_AUTH_WPA_PSK;
}

static void connect_to(const uint8_t *ssid, uint8_t ssid_len, const uint8_t *key, uint8_t key_len, uint8_t sec)
{
    wifi_config_t c = {0};
    memcpy(c.sta.ssid, ssid, ssid_len);
    memcpy(c.sta.password, key, key_len < 64 ? key_len : 63);
    c.sta.threshold.authmode = threshold(sec);
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK)
    {
        debug_printf("net: connect %d\n", e);
    }
}

/* Scans and connects to the stored network with the strongest signal. */
static void auto_connect(void)
{
    static wifi_ap_record_t rec[SETUP_SCAN_MAX];
    if (offline || mode != M_STATION || !profile_count() || state >= 3)
    {
        return;
    }
    wifi_scan_config_t sc = {.show_hidden = true};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
    {
        return;
    }
    uint16_t n = SETUP_SCAN_MAX;
    esp_wifi_scan_get_ap_records(&n, rec);
    int best = -1, best_rssi = -1000;
    for (int i = 0; i < 7; i++)
    {
        const profile_t *p = &profiles[i];
        for (unsigned k = 0; p->present && k < n; k++)
        {
            if (strlen((const char *)rec[k].ssid) == p->ssid_len && !memcmp(rec[k].ssid, p->ssid, p->ssid_len) &&
                rec[k].rssi > best_rssi)
            {
                best = i;
                best_rssi = rec[k].rssi;
            }
        }
    }
    if (best < 0)
    {
        debug_verbose("net: no stored network in range (%u found)\n", n);
        return;
    }
    const profile_t *p = &profiles[best];
    connecting = true;
    connect_to(p->ssid, p->ssid_len, p->key, p->key_len, p->sec);
}

/* --- start and shutdown --- */

static void wifi_up(wifi_mode_t m)
{
    esp_wifi_set_mode(m);
    esp_wifi_start();
    /* modem sleep between beacons, as ESP-IDF's default: the radio is the
     * biggest part of the idle current; it wakes for every DTIM beacon, so a
     * frame from the server waits at most one beacon interval or so. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    running = true;
}

static void net_down(const char *why)
{
    if (!running)
    {
        return;
    }
    uint32_t t0 = tick_ms();
    page_stop();
    handing_over = true;
    debug_printf("net: shutdown (%s)\n", why);
    log_drain();
    end_session();
    for (unsigned i = 0; log_connected(); i++)
    {
        if (i >= 5400)
        {
            panic(PANIC_WAIT, 54000, "network hand-over");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    set_state(0, why);
    running = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    handing_over = false;
    CRITICAL_ENTER();
    latest_set = false;
    CRITICAL_EXIT();
    debug_printf("net: shutdown done, %lu ms\n", (unsigned long)(tick_ms() - t0));
}

static void startup(void)
{
    /* the clock starts at the build time at the latest, which
     * the certificate checks also use. */
    if (clock_now() < CFW_BUILD_TIME)
    {
        clock_set(CFW_BUILD_TIME);
        struct timeval tv = {.tv_sec = CFW_BUILD_TIME};
        settimeofday(&tv, NULL);
    }
    esp_netif_init();
    esp_event_loop_create_default();
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    if (esp_wifi_init(&cfg) != ESP_OK)
    {
        panic(PANIC_NWP, 0, "wifi init");
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    profiles_load();
    net_import_original_profiles();
    unsigned n = profile_count();
    if (!n)
    {
        set_state(1, "no profile");
    }
    wifi_up(WIFI_MODE_STA);
    debug_printf("net: up, MAC %02x:%02x:%02x:%02x:%02x:%02x, %u profiles%s\n", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5], n, offline ? " offline" : "");
    settings_load();
    log_start(mac[5]);
    if (offline && ledui_offline)
    {
        ledui_offline(true);
    }
    ledui_request(LED_READY_IDLE);
    ledui_request(LED_CONFIRMATION);
    player_prompt(0, true);
}

/* --- offline mode --- */

static void offline_on(void)
{
    ledui_request(LED_READY_IDLE);
    if (ledui_offline)
    {
        ledui_offline(true);
    }
    ledui_request(LED_CONFIRMATION);
    offline = true;
    end_session();
    esp_wifi_disconnect();
    debug_printf("net: offline on\n");
    player_prompt(7, false);
}

static void offline_off(void)
{
    if (player_stop_prompt)
    {
        player_stop_prompt();
    }
    player_prompt(8, false);
    ledui_request(LED_READY_IDLE);
    if (ledui_offline)
    {
        ledui_offline(false);
    }
    ledui_request(LED_CONFIRMATION);
    offline = false;
    next_try = 0;
    debug_printf("net: offline off\n");
}

/* --- setup search and access point --- */

static void dns_open(void)
{
    dns_sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = lwip_htons(53)};
    if (dns_sock >= 0 && lwip_bind(dns_sock, (struct sockaddr *)&a, sizeof a) == 0)
    {
        lwip_fcntl(dns_sock, F_SETFL, O_NONBLOCK);
        return;
    }
    if (dns_sock >= 0)
    {
        lwip_close(dns_sock);
    }
    dns_sock = -1;
}

static void dns_close(void)
{
    if (dns_sock >= 0)
    {
        lwip_close(dns_sock);
    }
    dns_sock = -1;
}

/* Every A question answered with 192.168.1.1 (toniebox.start). */
static void dns_poll(void)
{
    uint8_t q[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n;
    while (dns_sock >= 0 && (n = lwip_recvfrom(dns_sock, q, sizeof q - 16, 0, (struct sockaddr *)&from, &fl)) > 12)
    {
        q[2] = 0x81; /* response, recursion desired */
        q[3] = 0x80; /* recursion available, no error */
        q[6] = 0;
        q[7] = 1; /* one answer */
        q[8] = q[9] = q[10] = q[11] = 0;
        static const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 1, 1};
        memcpy(q + n, ans, sizeof ans);
        lwip_sendto(dns_sock, q, (size_t)n + sizeof ans, 0, (struct sockaddr *)&from, fl);
        fl = sizeof from;
    }
}

static void open_ap(void)
{
    sound_chime(CHIME_SIGNAL);
    static const char hexd[] = "0123456789ABCDEF";
    wifi_config_t c = {0};
    char ssid[14] = "Toniebox-";
    ssid[9] = hexd[mac[4] >> 4];
    ssid[10] = hexd[mac[4] & 15];
    ssid[11] = hexd[mac[5] >> 4];
    ssid[12] = hexd[mac[5] & 15];
    memcpy(c.ap.ssid, ssid, 13);
    c.ap.ssid_len = 13;
    c.ap.channel = 6;
    c.ap.authmode = WIFI_AUTH_OPEN;
    c.ap.max_connection = 2;
    c.ap.beacon_interval = 100;
    net_down("setup mode");
    CRITICAL_ENTER();
    setup_creds_clear(&creds, settings_language());
    CRITICAL_EXIT();
    esp_netif_dhcps_stop(ap_if);
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 1, 1);
    IP4_ADDR(&ip.gw, 192, 168, 1, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(ap_if, &ip);
    esp_netif_dhcps_start(ap_if);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &c);
    wifi_up(WIFI_MODE_AP);
    dns_open();
    page_allowed = true;
    mode = M_SETUP;
    ledui_request(LED_SETUP);
    debug_printf("net: setup mode, %u networks\n", scan_n);
}

static void setup_search(void)
{
    static wifi_ap_record_t rec[SETUP_SCAN_MAX];
    scan_n = 0;
    mode = M_SEARCH;
    offline_req = false;
    debug_printf("net: setup search\n");
    ledui_request(LED_SEARCH);
    if (player_stop_all)
    {
        player_stop_all();
    }
    log_drain();
    end_session();
    sound_chime(CHIME_SIGNAL);
    esp_wifi_disconnect();
    uint16_t n = 0;
    /* about 10 s of scanning, the gesture can end it. */
    for (int round = 0; round < 4; round++)
    {
        wifi_scan_config_t sc = {.show_hidden = false};
        esp_wifi_scan_start(&sc, true);
        if (offline_req)
        {
            debug_printf("net: search ended (offline)\n");
            mode = M_STATION;
            offline_on();
            return;
        }
    }
    n = SETUP_SCAN_MAX;
    esp_wifi_scan_get_ap_records(&n, rec);
    for (unsigned i = 0; i < n; i++)
    {
        scan_entry_t e = {0};
        e.len = (uint8_t)strnlen((const char *)rec[i].ssid, 32);
        memcpy(e.ssid, rec[i].ssid, e.len);
        e.sec = rec[i].authmode == WIFI_AUTH_OPEN ? 0 : rec[i].authmode == WIFI_AUTH_WEP   ? 1
                                                    : rec[i].authmode == WIFI_AUTH_WPA_PSK ? 2
                                                                                           : 3;
        e.rssi = rec[i].rssi;
        scan[i] = e;
    }
    scan_n = setup_scan_process(scan, n);
    open_ap();
}

static void leave_setup(void)
{
    sound_chime(CHIME_SIGNAL);
    page_stop();
    dns_close();
    net_down("leave setup");
    wifi_up(WIFI_MODE_STA);
    mode = M_STATION;
    next_try = 0;
    ledui_request(LED_READY_IDLE);
    ledui_request(LED_CONFIRMATION);
}

/* A new search from setup mode, asked for by the setup page: the scan needs
 * the station mode; the setup search opens the access point again. */
static void rescan(void)
{
    debug_printf("net: setup rescan\n");
    dns_close();
    net_down("rescan");
    wifi_up(WIFI_MODE_STA);
    setup_search();
}

static void setup_gesture(void)
{
    if (going_down)
    {
        return;
    }
    if (offline)
    {
        offline_off();
        return;
    }
    profile_pending = false;
    if (mode == M_SETUP)
    {
        leave_setup();
    }
    else
    {
        setup_search();
    }
}

/* --- join attempt --- */

static void error_indication(unsigned prompt)
{
    ledui_request(LED_ERROR);
    player_prompt(prompt, false);
}

static void join(uint8_t language)
{
    static uint8_t ssid[32], key[64];
    CRITICAL_ENTER();
    uint8_t ssid_len = creds.ssid_len, key_len = creds.key_len, m = creds.mode;
    memcpy(ssid, creds.ssid, ssid_len);
    memcpy(key, creds.key, key_len);
    CRITICAL_EXIT();
    if (language != settings_language())
    {
        settings_set_language(language);
    }
    sound_chime(CHIME_SIGNAL);
    ledui_request(LED_CONNECTING);
    mode = M_JOIN;
    join_handover = true;
    join_conn = join_addr = false;
    join_waiting = true;
    page_stop();
    dns_close();
    net_down("join");
    wifi_up(WIFI_MODE_STA);
    join_handover = false;
    connect_to(ssid, ssid_len, key, key_len, setup_join_security(m));
    uint32_t t0 = tick_ms();
    while (!(join_conn && join_addr) && tick_ms() - t0 < 10000)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    bool conn = join_conn, addr = join_addr;
    join_waiting = false;
    log_fn(LOG_INFO, 24, 163, 0, NULL);
    mode = M_STATION;
    if (conn && addr)
    {
        debug_printf("net: join ok\n");
        memset(&pending, 0, sizeof pending);
        pending.ssid_len = ssid_len;
        memcpy(pending.ssid, ssid, ssid_len);
        pending.key_len = key_len;
        memcpy(pending.key, key, key_len);
        pending.sec = setup_profile_security(m);
        profile_pending = true;
        player_prompt(1, false);
        ledui_request(LED_READY_IDLE);
        ledui_request(LED_CONFIRMATION);
        cloud_sync_request(false);
        return;
    }
    debug_printf("net: join failed (%s)\n", conn ? "no address" : "not associated");
    esp_wifi_disconnect();
    next_try = 0;
    ledui_request(LED_READY_IDLE);
    error_indication(conn ? 20 : 19);
}

static void cloud_result(uint32_t s, int error)
{
    if (!profile_pending)
    {
        return;
    }
    profile_pending = false;
    if (state <= 3)
    {
        debug_printf("net: profile dropped (state %u)\n", state);
        return;
    }
    switch (error)
    {
    case CE_OK:
        net_cloud_state(s, 6);
        store_profile();
        break;
    case CE_NAME:
    case CE_CONNECT:
    case CE_TRANSFER:
    case CE_TIMEOUT:
        ledui_request(LED_NETWORK_ERROR);
        player_prompt(error == CE_NAME ? 21 : 23, false);
        ready_at = tick_ms() + 15000;
        if (!ready_at)
        {
            ready_at = 1;
        }
        break;
    default:
        debug_printf("net: profile dropped (%d)\n", error);
        break;
    }
}

static void handle(const event_t *e)
{
    switch (e->kind)
    {
    case EV_DISCONNECT:
        end_session();
        connecting = false;
        debug_printf("net: disconnected, reason %u\n", e->len);
        if (mode != M_STATION && mode != M_JOIN)
        {
            break;
        }
        set_state(profile_count() ? 0 : 1, "disconnection");
        next_try = tick_ms() + 2000;
        break;
    case EV_CONNECT:
    {
        end_session();
        char ssid[33];
        memcpy(ssid, e->ssid, e->len);
        ssid[e->len] = 0;
        debug_printf("net: connected to \"%s\"\n", ssid);
        if (!offline)
        {
            set_state(3, "connection");
        }
        break;
    }
    case EV_ADDRESS:
    {
        uint32_t ip = e->ip, gw = e->gateway;
        debug_printf("net: address %lu.%lu.%lu.%lu, gateway %lu.%lu.%lu.%lu\n", (unsigned long)(ip >> 24),
                     (unsigned long)(ip >> 16 & 255), (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255),
                     (unsigned long)(gw >> 24), (unsigned long)(gw >> 16 & 255), (unsigned long)(gw >> 8 & 255),
                     (unsigned long)(gw & 255));
        end_session();
        connecting = false;
        if (offline || ip == gw || state > 3)
        {
            break;
        }
        session = ++session_count;
        set_state(4, "address");
        cloud_online_round(session);
        break;
    }
    case EV_SHUTDOWN:
        dns_close();
        net_down("power");
        xSemaphoreGive(done);
        break;
    case EV_SETUP:
        setup_gesture();
        break;
    case EV_RESCAN:
        if (mode == M_SETUP && !going_down)
        {
            rescan();
        }
        break;
    case EV_SUBMIT:
        if (mode == M_SETUP && !player_busy() && !going_down)
        {
            join(e->len);
        }
        break;
    case EV_CLOUD_RESULT:
        cloud_result(e->ip, (int)e->info);
        break;
    }
}

static void task(void *arg)
{
    (void)arg;
    startup();
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(mode == M_SETUP || ready_at ? 100 : 1000));
        event_t e;
        while (xQueueReceive(queue, &e, 0) == pdTRUE)
        {
            handle(&e);
        }
        CRITICAL_ENTER();
        bool have = latest_set;
        if (have)
        {
            e = latest;
        }
        latest_set = false;
        CRITICAL_EXIT();
        if (have && !going_down)
        {
            handle(&e);
        }
        if (mode == M_SETUP)
        {
            dns_poll();
        }
        if (!going_down && running && mode == M_STATION && !connecting && state < 3 &&
            (int32_t)(tick_ms() - next_try) >= 0)
        {
            auto_connect();
            next_try = tick_ms() + (connecting ? 15000 : 10000);
            connecting = false;
        }
        if (cloud_restart_wanted())
        {
            debug_printf("net: no buffers, reconnect\n");
            esp_wifi_disconnect();
        }
        if (ready_at && (int32_t)(tick_ms() - ready_at) >= 0)
        {
            ready_at = 0;
            ledui_request(LED_READY_IDLE);
        }
    }
}

void net_setup_request(void)
{
    post_request(EV_SETUP, 0, 0);
}

void net_setup_rescan(void)
{
    post_request(EV_RESCAN, 0, 0);
}

bool net_sync_gesture(void)
{
    if (mode == M_SEARCH)
    {
        offline_req = true;
        esp_wifi_scan_stop();
        return true;
    }
    return mode == M_SETUP;
}

void net_cloud_first_result(uint32_t s, int error)
{
    if (profile_pending)
    {
        post_request(EV_CLOUD_RESULT, s, (uint32_t)error);
    }
}

void net_shutdown(void)
{
    going_down = true;
    if (!net_task || xTaskGetCurrentTaskHandle() == net_task)
    {
        net_down("power");
        return;
    }
    event_t e = {0};
    e.kind = EV_SHUTDOWN;
    while (xQueueSend(queue, &e, pdMS_TO_TICKS(100)) != pdTRUE)
        ;
    xTaskNotifyGive(net_task);
    do
    {
        monitor_feed();
    } while (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE);
}

void net_start(void)
{
    static StackType_t stack[TASK_STACK(768)];
    static StaticTask_t tcb;
    queue = xQueueCreateStatic(8, sizeof(event_t), queue_buf, &queue_mem);
    done = xSemaphoreCreateBinaryStatic(&done_mem);
    net_task = xTaskCreateStatic(task, "net", TASK_STACK(768), NULL, 3, stack, &tcb);
}
