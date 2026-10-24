/*
 * Network processor and Wi-Fi in the station role.
 *
 * Setup search, setup mode and the join attempt (the data rules are in
 * setup.c) and offline mode; no factory reset (the setup page deletes
 * the profiles instead). The hand-over
 * of the network shutdown (step 2) waits until the log connection is closed and no
 * other task uses the driver (osi_driver_busy).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "simplelink.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "clock.h"
#include "cloud.h"
#include "debug.h"
#include "net.h"
#include "ledui.h"
#include "monitor.h"
#include "osi.h"
#include "panic.h"
#include "player.h"
#include "rtnl.h"
#include "settings.h"
#include "setup.h"
#include "sound.h"
#include "tick.h"
#include "version.h"

/* Set by spi_Open (cc_pal.c): the SPI to the network processor runs on uDMA. */
extern unsigned char g_ucDMAEnabled;

/* Not in the driver's netapp.h (commented out there). */
#define NET_APP_DNS_SERVER_ID 8
/* SimpleLink protocol.h: SL_OPCODE_NETAPP_STOP_COMMAND. */
#define OPCODE_NETAPP_STOP 0x9C61

/* Provided by other modules once they implement their part; until then
 * the step is skipped (weak, NULL when absent). */
void player_stop_all(void) __attribute__((weak));    /* stop content and sound prompt */
void player_stop_prompt(void) __attribute__((weak)); /* stop a playing sound prompt */
void ledui_offline(bool on) __attribute__((weak));   /* white ready colour */
void settings_set_language(uint8_t l) __attribute__((weak));
bool settings_country(char cc[2]) __attribute__((weak));

/* The network events (only the latest waiting one is kept)
 * and the other requests (kept in order, handled first). */
typedef enum
{
    EV_DISCONNECT,
    EV_CONNECT,
    EV_LINK_FAULT,
    EV_ADDRESS,
    EV_SUBMIT, /* network events */
    EV_FAULT,
    EV_ERROR,
    EV_SHUTDOWN,
    EV_SETUP,
    EV_RESCAN,
    EV_CLOUD_RESULT,
} ev_t;

typedef struct
{
    uint8_t kind;
    uint8_t len;  /* SSID length, disconnection reason, setup language */
    bool running; /* EV_FAULT: the network processor counted as running */
    uint8_t ssid[32];
    uint32_t ip, gateway;
    uint32_t info; /* EV_FAULT: driver event and its info; EV_ERROR: error word */
} event_t;

/* Shutdown reasons (record 8339, implementation-defined). */
enum
{
    R_POWER = 1,
    R_ROLE = 2,
    R_POLICY = 3,
    R_FAULT = 4,
    R_SERVICES = 5,
    R_BUFFERS = 6,
    R_SETUP = 7,
    R_OFFLINE = 8
};

/* What the net activity is doing (box activities). */
typedef enum
{
    M_STATION,
    M_SEARCH,
    M_SETUP,
    M_JOIN
} net_mode_t;

static StaticQueue_t queue_mem;
static uint8_t queue_buf[8 * sizeof(event_t)];
static QueueHandle_t queue;
static TaskHandle_t net_task;
static event_t latest;
static volatile bool latest_set;
static volatile uint32_t replaced, lost;

static volatile uint8_t state;
static volatile uint32_t session, session_count;
static volatile uint32_t sp_version;
/* Events are taken from the start of the network processor on (the
 * auto-connect can join before sl_Start has returned), and ignored only
 * while it is stopped. */
static volatile bool running;
static volatile bool addr_seen;  /* an address event since the start */
static volatile bool going_down; /* net_shutdown: no restarts any more */
static volatile bool handing_over;
static bool nwp_on;
static long role = -1;
static uint8_t mac[6];
static uint32_t link_fault_at, shutdowns;
static StaticSemaphore_t done_mem;
static SemaphoreHandle_t done;
/* Driver faults since the firmware started, and the last
 * one: shown with a fatal error, which a fault usually causes. */
static volatile uint32_t faults, fault_ev, fault_info;
static uint8_t policy; /* connection policy last read */

static volatile uint8_t mode; /* net_mode_t */
static volatile bool offline;
static volatile bool offline_req; /* offline gesture during the setup search */
static bool setup_opened;         /* since the firmware started */
static volatile bool join_waiting, join_conn, join_addr, join_handover;
static setup_creds_t creds; /* written by the token events */
static volatile unsigned scan_n;
static volatile uint32_t post_logged, form_failed;
/* Setup page served by the cloud activity (page.c): allowed
 * while the access point is open, active while page_run serves, took when
 * it stopped the network processor's HTTP server. */
static volatile bool page_allowed, page_active, page_took;
/* the credentials of a successful join until the first cloud
 * exchange. */
static volatile bool profile_pending;
static uint8_t prof_ssid[32], prof_ssid_len, prof_key[64], prof_key_len, prof_sec;
static uint32_t ready_at; /* ready display after a failure indication */
/* Work memory of the net activity: the raw scan list during the setup
 * search, converted in place to the processed list offered in setup mode; the
 * slots and a profile name for a profile storage, which
 * comes after setup mode has been left, and the profile name for the
 * profile listing at start, before any setup search. All uses are in the
 * net activity, one after the other. */
static union
{
    Sl_WlanNetworkEntry_t raw[SETUP_SCAN_MAX];
    scan_entry_t list[SETUP_SCAN_MAX];
    struct
    {
        slot_info_t slots[7];
        int8_t name[256]; /* up to 255 name bytes */
    } prof;
} work;
#define scan work.list
#define profile_name work.prof.name

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
    return sp_version;
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

/* The name power.c uses. */
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
    taskENTER_CRITICAL();
    uint8_t was = state;
    state = s;
    taskEXIT_CRITICAL();
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
    taskENTER_CRITICAL();
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
    taskEXIT_CRITICAL();
    if (!ok || !step)
    {
        debug_verbose("net: cloud state %u ignored (state %u, session %lu/%lu)\n", to, was,
                      (unsigned long)s, (unsigned long)session);
    }
    else if (was != to)
    {
        debug_printf("net: state %u -> %u (cloud)\n", was, to);
    }
}

/* --- driver callbacks: spawn task, or the task in which the driver found
 * a fault. They only copy and post (384-word spawn stack). --- */

static void post_net(const event_t *e)
{
    taskENTER_CRITICAL();
    if (latest_set)
    {
        replaced++;
    }
    latest = *e;
    latest_set = true;
    taskEXIT_CRITICAL();
    if (net_task)
    {
        xTaskNotifyGive(net_task);
    }
}

static void post_other(const event_t *e)
{
    if (xQueueSend(queue, e, 0) != pdTRUE)
    {
        lost++;
    }
    if (net_task)
    {
        xTaskNotifyGive(net_task);
    }
}

void SimpleLinkWlanEventHandler(SlWlanEvent_t *e)
{
    event_t ev = {0};
    if (!running || !e)
    {
        return;
    }
    switch (e->Event)
    {
    case SL_WLAN_CONNECT_EVENT:
        ev.kind = EV_CONNECT;
        ev.len = e->EventData.STAandP2PModeWlanConnected.ssid_len;
        if (ev.len <= 32)
        {
            memcpy(ev.ssid, e->EventData.STAandP2PModeWlanConnected.ssid_name, ev.len);
        }
        else
        {
            ev.len = 0;
        }
        if (join_waiting)
        {
            join_conn = true;
        }
        post_net(&ev);
        break;
    case SL_WLAN_DISCONNECT_EVENT:
        ev.kind = EV_DISCONNECT;
        ev.len = e->EventData.STAandP2PModeDisconnected.reason_code;
        post_net(&ev);
        break;
    default:
        break; /* access-point events: ignored */
    }
}

void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *e)
{
    if (!running || !e)
    {
        return;
    }
    if (e->Event == SL_NETAPP_IPV4_IPACQUIRED_EVENT)
    {
        event_t ev = {EV_ADDRESS, 0, false, {0}, e->EventData.ipAcquiredV4.ip, e->EventData.ipAcquiredV4.gateway, 0};
        addr_seen = true;
        if (join_waiting && ev.ip != ev.gateway)
        {
            join_addr = true;
        }
        post_net(&ev);
    }
}

void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *e)
{
    event_t ev = {0};
    if (!e)
    {
        return;
    }
    if (e->Event == SL_DEVICE_GENERAL_ERROR_EVENT)
    {
        uint16_t word = (uint8_t)e->EventData.deviceEvent.status |
                        (uint16_t)((uint8_t)e->EventData.deviceEvent.sender << 8);
        if (word == 0x02C3 || !running)
        {
            return;
        }
        ev.kind = word == 0x0593 ? EV_LINK_FAULT : EV_ERROR;
        ev.info = word;
        if (ev.kind == EV_LINK_FAULT)
        {
            post_net(&ev);
        }
        else
        {
            post_other(&ev);
        }
        return;
    }
    /* Abort, driver assert, command-completion, sync-pattern or event
     * time-out: every command fails from now on until the restart. */
    ev.kind = EV_FAULT;
    ev.running = running;
    ev.ip = (uint32_t)e->Event;
    ev.info = e->Event == SL_DEVICE_ABORT_ERROR_EVENT ? e->EventData.deviceReport.AbortType
                                                      : e->EventData.deviceDriverReport.info;
    fault_ev = (uint32_t)e->Event;
    fault_info = ev.info;
    faults++;
    post_other(&ev);
}

void SimpleLinkSockEventHandler(SlSockEvent_t *e)
{
    (void)e;
}

/* One POST field: from the token events and
 * from the setup page (page.c). The values only store bytes; a submit is
 * posted to the net activity. */
void net_setup_field(const uint8_t *name, unsigned name_len, const uint8_t *value, unsigned value_len)
{
    taskENTER_CRITICAL();
    setup_post_t r = setup_post(&creds, name, name_len, value, value_len);
    bool submit = r == POST_SUBMIT && !join_waiting;
    bool ok = submit && setup_form(&creds);
    uint8_t lang = creds.language;
    taskEXIT_CRITICAL();
    if (r == POST_LOGGED)
    {
        post_logged++;
    }
    if (submit && !ok)
    {
        form_failed++;
    }
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

/* GET tokens are answered from the processed
 * scan list (fixed while the access point is open). */
void SimpleLinkHttpServerCallback(SlHttpServerEvent_t *ev, SlHttpServerResponse_t *resp)
{
    if (!ev || !running)
    {
        return;
    }
    if (ev->Event == SL_NETAPP_HTTPGETTOKENVALUE_EVENT && resp)
    {
        resp->Response = SL_NETAPP_HTTPSETTOKENVALUE;
        resp->ResponseData.token_value.len =
            (uint8_t)setup_get(scan, scan_n, ev->EventData.httpTokenName.data, ev->EventData.httpTokenName.len,
                               resp->ResponseData.token_value.data);
    }
    else if (ev->Event == SL_NETAPP_HTTPPOSTTOKENVALUE_EVENT)
    {
        const slHttpServerPostData_t *d = &ev->EventData.httpPostData;
        net_setup_field(d->token_name.data, d->token_name.len, d->token_value.data, d->token_value.len);
    }
}

/* --- setup page --- */

bool net_page_begin(void)
{
    taskENTER_CRITICAL();
    bool ok = mode == M_SETUP && page_allowed && !page_active;
    if (ok)
    {
        page_active = true;
    }
    taskEXIT_CRITICAL();
    return ok;
}

bool net_page_on(void)
{
    return page_allowed;
}

void net_page_end(bool took)
{
    if (took)
    {
        page_took = true;
    }
    page_active = false;
}

/* Ends the page server (at most 10 s); true when it had stopped the HTTP
 * server, which then needs no stop command. */
static bool page_stop(void)
{
    page_allowed = false;
    for (unsigned i = 0; page_active && i < 1000; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (page_active)
    {
        debug_printf("net: page busy\n");
    }
    bool t = page_took;
    page_took = false;
    return t;
}

/* --- start and shutdown --- */

/* Fatal network processor error. */
static void __attribute__((noreturn)) fatal(const char *what, long r)
{
    debug_printf("net: %s %ld FATAL, %lu, last %lu/0x%lx\n", what, r, (unsigned long)faults,
                 (unsigned long)fault_ev, (unsigned long)fault_info);
    panic(PANIC_NWP, (uint32_t)r, what);
}

static long nwp_start(bool first)
{
    log_fn(LOG_INFO, 24, 35, 0, NULL);
    addr_seen = false;
    running = true;
    uint32_t t0 = tick_ms(), f0 = faults;
    long r = sl_Start(NULL, NULL, NULL);
    debug_printf("net: sl_Start %ld, %lu ms, dma %u\n", r, (unsigned long)(tick_ms() - t0), g_ucDMAEnabled);
    /* A start whose completion timed out returns a role taken from an
     * answer that never came: the fault event tells it. */
    if (r < 0 || faults != f0)
    {
        fatal("start", r);
    }
    nwp_on = true;
    role = r;
    /* P2P or an unknown role only at the first start; events
     * are then ignored until the switch. */
    if (r != ROLE_STA && r != ROLE_AP)
    {
        if (!first)
        {
            fatal("role", r);
        }
        running = false;
    }
    return r;
}

static void after_start(long r_role, bool first)
{
    SlDateTime_t dt;
    uint8_t opt = SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME, len = sizeof dt;
    long r = sl_DevGet(SL_DEVICE_GENERAL_CONFIGURATION, &opt, &len, (uint8_t *)&dt);
    if (r < 0)
    {
        fatal("date read", r);
    }
    civil_t c = {dt.sl_tm_year, dt.sl_tm_mon, dt.sl_tm_day, dt.sl_tm_hour, dt.sl_tm_min, dt.sl_tm_sec};
    uint32_t nwp = clock_from_civil(&c), now = nwp;
    if (nwp < CFW_BUILD_TIME)
    {
        clock_to_civil(CFW_BUILD_TIME, &c);
        dt.sl_tm_year = c.year;
        dt.sl_tm_mon = c.month;
        dt.sl_tm_day = c.day;
        dt.sl_tm_hour = c.hour;
        dt.sl_tm_min = c.minute;
        dt.sl_tm_sec = c.second;
        r = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION, SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME, sizeof dt,
                      (uint8_t *)&dt);
        if (r < 0)
        {
            fatal("date write", r);
        }
        now = CFW_BUILD_TIME;
    }
    clock_set(now);
    debug_verbose("net: NWP date %lu, base %lu\n", (unsigned long)nwp, (unsigned long)now);

    if (r_role == ROLE_AP)
    {
        /* Step 2: the access point's own address, without a time limit. */
        for (unsigned i = 0; !addr_seen; i++)
        {
            if (i == 500)
            {
                debug_verbose("net: AP address wait\n");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    else if (r_role == ROLE_STA)
    {
        r = sl_WlanPolicySet(SL_POLICY_SCAN, SL_SCAN_POLICY(0), NULL, 0);
        if (r < 0)
        {
            fatal("scan policy", r);
        }
    }
    if (first)
    {
        SlVersionFull v;
        opt = SL_DEVICE_GENERAL_VERSION;
        len = sizeof v;
        r = sl_DevGet(SL_DEVICE_GENERAL_CONFIGURATION, &opt, &len, (uint8_t *)&v);
        if (r < 0)
        {
            fatal("version read", r);
        }
        sp_version = (v.NwpVersion[0] & 0xFF) << 24 | (v.NwpVersion[1] & 0xFF) << 16 |
                     (v.NwpVersion[2] & 0xFF) << 8 | (v.NwpVersion[3] & 0xFF);
        debug_verbose("net: chip %08lx, fw %lu.%lu.%lu.%lu, ROM %u\n", (unsigned long)v.ChipFwAndPhyVersion.ChipId,
                      (unsigned long)v.ChipFwAndPhyVersion.FwVersion[0],
                      (unsigned long)v.ChipFwAndPhyVersion.FwVersion[1],
                      (unsigned long)v.ChipFwAndPhyVersion.FwVersion[2],
                      (unsigned long)v.ChipFwAndPhyVersion.FwVersion[3], v.RomVersion);
    }
}

/* Network shutdown; nothing when the network
 * processor is already stopped. */
static void shutdown(uint8_t reason, const char *why)
{
    if (!nwp_on)
    {
        return;
    }
    uint32_t t0 = tick_ms();
    page_stop();
    handing_over = true;
    debug_printf("net: shutdown (%s)\n", why);
    log_drain();
    log_args_t a = {0};
    log_arg_num(&a, reason);
    log_arg_num(&a, (int32_t)++shutdowns);
    log_fn(LOG_INFO, 24, 8339, 0, &a);
    end_session();
    /* The cloud and log work ends at its next check of the session; then
     * no other task is inside the driver (at most 54 s). */
    for (unsigned i = 0; log_connected() || osi_driver_busy(); i++)
    {
        if (i == 100 || i == 5400)
        {
            debug_verbose("net: hand-over log %d drv %d\n", log_connected(), osi_driver_busy());
        }
        if (i >= 5400)
        {
            panic(PANIC_WAIT, 54000, "network hand-over");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    set_state(0, why);
    running = false;
    /* After a driver fault the driver sends no stop command and only
     * powers the network processor down. A completion time-out is only
     * logged (the fault event arrives while stopped). */
    long r = sl_Stop(200);
    if (r < 0)
    {
        fatal("stop", r);
    }
    handing_over = false;
    vTaskDelay(pdMS_TO_TICKS(60));
    nwp_on = false;
    role = -1;
    /* Reports of the stopped instance are not handled. */
    taskENTER_CRITICAL();
    latest_set = false;
    taskEXIT_CRITICAL();
    event_t e;
    for (UBaseType_t n = uxQueueMessagesWaiting(queue); n; n--)
    {
        if (xQueueReceive(queue, &e, 0) == pdTRUE && e.kind != EV_FAULT && e.kind != EV_ERROR)
        {
            xQueueSend(queue, &e, 0); /* requests stay, in order */
        }
    }
    uint32_t took = tick_ms() - t0;
    a.n = 0;
    log_arg_num(&a, reason);
    log_arg_num(&a, (int32_t)took);
    log_fn(took >= 1000 ? LOG_WARN : LOG_INFO, 24, 8372, took >= 1000 ? 1 : 0, &a);
    debug_verbose("net: shutdown done, %lu ms\n", (unsigned long)took);
}

/* Network restart. */
static long restart(uint8_t reason, const char *why)
{
    shutdown(reason, why);
    long r = nwp_start(false);
    after_start(r, false);
    return r;
}

/* the restarts after a driver fault or a lack of buffers only
 * while running in the station or AP role. */
static void restart_if_running(uint8_t reason, const char *why)
{
    if (going_down || !nwp_on || (role != ROLE_STA && role != ROLE_AP))
    {
        debug_printf("net: no restart %s, role %ld\n", why, role);
        return;
    }
    restart(reason, why);
}

/* false when a stop failed; the remaining ones are skipped. */
static bool stop_services(void)
{
    static const uint32_t id[] = {SL_NET_APP_DHCP_SERVER_ID, NET_APP_DNS_SERVER_ID, SL_NET_APP_HTTP_SERVER_ID};
    for (unsigned i = 0; i < sizeof id / sizeof id[0]; i++)
    {
        if (setup_opened)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (id[i] == SL_NET_APP_HTTP_SERVER_ID && page_stop())
        {
            continue; /* the page server had stopped it */
        }
        long r = sl_NetAppStop(id[i]);
        debug_verbose("net: stop svc %lu: %ld\n", (unsigned long)id[i], r);
        if (r < 0)
        {
            debug_printf("net: svc %lu %ld -> restart\n", (unsigned long)id[i], r);
            return false;
        }
    }
    return true;
}

/* auto-connect (on) or off; true when the policy had to be
 * changed. */
static bool set_policy(bool on)
{
    uint8_t pol = 0, len = 1;
    long r = sl_WlanPolicyGet(SL_POLICY_CONNECTION, 0, &pol, &len);
    if (r < 0)
    {
        fatal("policy get", r);
    }
    policy = pol;
    debug_verbose("net: policy 0x%02x, want %d\n", pol, on);
    if (on ? pol != 0 : pol == 0)
    {
        return false;
    }
    debug_verbose("net: policy 0x%02x -> %s\n", pol, on ? "auto" : "off");
    r = sl_WlanPolicySet(SL_POLICY_CONNECTION, on ? SL_CONNECTION_POLICY(1, 0, 0, 0, 0) : SL_CONNECTION_POLICY(0, 0, 0, 0, 0),
                         NULL, 0);
    if (r < 0)
    {
        fatal("policy set", r);
    }
    policy = on;
    return true;
}

/* Stops the services; a network restart when a stop failed. */
static void stop_services_or_restart(void)
{
    if (!stop_services())
    {
        restart(R_SERVICES, "svc stop");
    }
}

static void role_station_restart(uint8_t reason, const char *why)
{
    long r = sl_WlanSetMode(ROLE_STA);
    if (r < 0)
    {
        fatal("role switch", r);
    }
    restart(reason, why);
}

static void power_policy(void)
{
    uint8_t v = 0, len = 1;
    long r = sl_WlanPolicyGet(SL_POLICY_PM, 0, &v, &len);
    if (r < 0)
    {
        fatal("PM get", r);
    }
    debug_verbose("net: power policy %u\n", v);
    if (v != SL_NORMAL_POLICY)
    {
        return;
    }
    r = sl_WlanPolicySet(SL_POLICY_PM, SL_ALWAYS_ON_POLICY, NULL, 0);
    if (r < 0)
    {
        fatal("PM set", r);
    }
    len = 1;
    r = sl_WlanPolicyGet(SL_POLICY_PM, 0, &v, &len);
    if (r < 0)
    {
        fatal("PM get", r);
    }
    if (v != 15)
    {
        debug_verbose("net: power policy read back as %u\n", v);
    }
}

static void startup(void)
{
    long r_role = nwp_start(true);
    after_start(r_role, true);

    if (!stop_services())
    {
        r_role = restart(R_SERVICES, "svc stop");
    }
    if (r_role != ROLE_STA)
    {
        long r = sl_WlanSetMode(ROLE_STA);
        if (r < 0)
        {
            fatal("role switch", r);
        }
        r_role = restart(R_ROLE, "to station");
    }
    /* Step 3: the offline flag. */
    if (set_policy(!offline))
    {
        r_role = restart(R_POLICY, "policy");
    }
    if (r_role != ROLE_STA)
    {
        fatal("role", r_role);
    }

    unsigned profiles = 0;
    for (int i = 0; i < 7; i++)
    {
        int8_t *name = profile_name;
        int16_t len = 0;
        uint8_t bssid[6];
        SlSecParams_t sec;
        uint32_t prio;
        if (sl_WlanProfileGet(i, name, &len, bssid, &sec, NULL, &prio) >= 0)
        {
            profiles++;
            if (len < 0 || len > 32)
            {
                len = 0;
            }
            name[len] = 0;
            debug_verbose("net: profile %d \"%s\", security %u, priority %lu\n", i, (char *)name, sec.Type,
                          (unsigned long)prio);
        }
    }
    if (!profiles)
    {
        set_state(1, "no profile");
    }

    uint8_t mac_len = sizeof mac;
    long r = sl_NetCfgGet(SL_MAC_ADDRESS_GET, NULL, &mac_len, mac);
    if (r < 0)
    {
        fatal("MAC get", r);
    }
    /* The summary of the network start-up, once per start. */
    debug_printf("net: up, NWP %lu.%lu.%lu.%lu, MAC %02x:%02x:%02x:%02x:%02x:%02x, %u prof, pol 0x%02x%s, "
                 "%lu rst\n",
                 (unsigned long)(sp_version >> 24), (unsigned long)(sp_version >> 16 & 255),
                 (unsigned long)(sp_version >> 8 & 255), (unsigned long)(sp_version & 255), mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5], profiles, policy, offline ? " offline" : "", (unsigned long)shutdowns);
    settings_load();
    log_start(mac[5]);
    /* LED lit since the start; a wait of
     * 1000 ms and sound prompt 0. */
    if (offline && ledui_offline)
    {
        ledui_offline(true);
    }
    ledui_request(LED_READY_IDLE);
    ledui_request(LED_CONFIRMATION);
    player_prompt(0, true);
}

/* --- offline mode --- */

/* Offline mode, after the setup search was ended and the scan switched off. */
static void offline_on(void)
{
    ledui_request(LED_READY_IDLE);
    if (ledui_offline)
    {
        ledui_offline(true);
    }
    bool changed = set_policy(false);
    ledui_request(LED_CONFIRMATION);
    offline = true;
    debug_verbose("net: offline on\n");
    if (changed)
    {
        restart(R_OFFLINE, "offline on");
    }
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
    bool changed = set_policy(true);
    ledui_request(LED_CONFIRMATION);
    offline = false;
    debug_verbose("net: offline off\n");
    if (changed)
    {
        restart(R_OFFLINE, "offline off");
    }
}

/* --- setup search and access point --- */

static void scan_policy(bool on)
{
    uint32_t interval = 1; /* s */
    long r = on ? sl_WlanPolicySet(SL_POLICY_SCAN, SL_SCAN_POLICY(1), (uint8_t *)&interval, sizeof interval)
                : sl_WlanPolicySet(SL_POLICY_SCAN, SL_SCAN_POLICY(0), NULL, 0);
    if (r < 0)
    {
        fatal("scan policy", r);
    }
}

/* read, compare over the new value's length, write if
 * different. */
static void wlan_setting(uint16_t group, uint16_t opt, const void *val, uint16_t len)
{
    uint8_t cur[36] = {0};
    uint16_t o = opt, l = sizeof cur;
    long r = sl_WlanGet(group, &o, &l, cur);
    if (r >= 0 && memcmp(cur, val, len))
    {
        debug_verbose("net: AP cfg %u/%u set\n", group, opt);
        r = sl_WlanSet(group, opt, len, (const uint8_t *)val);
    }
    if (r < 0)
    {
        /* options 2 and 5 of the AP group: numbers inferred */
        debug_printf("net: AP cfg %u/%u\n", group, opt);
        fatal("AP cfg", r);
    }
}

static void ap_configure(void)
{
    static const char hexd[] = "0123456789ABCDEF";
    uint8_t dtim = 1, channel = 6, power = 2;
    uint8_t beacon[2] = {16, 0};
    char ssid[13] = "Toniebox-";
    ssid[9] = hexd[mac[4] >> 4];
    ssid[10] = hexd[mac[4] & 15];
    ssid[11] = hexd[mac[5] >> 4];
    ssid[12] = hexd[mac[5] & 15];
    wlan_setting(SL_WLAN_CFG_AP_ID, 5, &dtim, 1);  /* DTIM period */
    wlan_setting(SL_WLAN_CFG_AP_ID, 2, beacon, 2); /* beacon interval */
    wlan_setting(SL_WLAN_CFG_AP_ID, WLAN_AP_OPT_CHANNEL, &channel, 1);
    wlan_setting(SL_WLAN_CFG_GENERAL_PARAM_ID, WLAN_GENERAL_PARAM_OPT_AP_TX_POWER, &power, 1);
    wlan_setting(SL_WLAN_CFG_AP_ID, WLAN_AP_OPT_SSID, ssid, sizeof ssid);
    static const char domain[] = "toniebox.start";
    long r = sl_NetAppSet(SL_NET_APP_DEVICE_CONFIG_ID, NETAPP_SET_GET_DEV_CONF_OPT_DOMAIN_NAME, sizeof domain - 1,
                          (const uint8_t *)domain);
    if (r < 0)
    {
        fatal("domain set", r);
    }
}

static void open_ap(void)
{
    sound_chime(CHIME_SIGNAL);
    if (!nwp_on)
    {
        long r = nwp_start(false);
        after_start(r, false);
    }
    ap_configure();
    long r = sl_WlanSetMode(ROLE_AP);
    if (r < 0)
    {
        fatal("role switch", r);
    }
    shutdown(R_SETUP, "setup mode");
    taskENTER_CRITICAL();
    setup_creds_clear(&creds, settings_language());
    taskEXIT_CRITICAL();
    r = nwp_start(false);
    after_start(r, false); /* waits for the access point's own address */
    if (r != ROLE_AP)
    {
        fatal("AP role", r);
    }
    r = sl_NetAppStop(0x0B);
    if (r < 0)
    {
        fatal("svc stop", r);
    }
    r = sl_NetAppStart(0x0B);
    if (r < 0)
    {
        fatal("svc start", r);
    }
    setup_opened = true;
    page_allowed = true;
    mode = M_SETUP;
    ledui_request(LED_SETUP);
    debug_printf("net: setup, %u networks\n", scan_n);
}

static void setup_search(void)
{
    Sl_WlanNetworkEntry_t *raw = work.raw;
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
    scan_policy(true);
    long n = 0;
    TickType_t t = xTaskGetTickCount();
    for (int w = 0; w < 10; w++)
    {
        for (int i = 0; i < 100; i++)
        {
            xTaskDelayUntil(&t, pdMS_TO_TICKS(20));
            if (offline_req)
            {
                debug_printf("net: search end, offline\n");
                scan_policy(false);
                mode = M_STATION;
                offline_on();
                return;
            }
        }
        n = sl_WlanGetNetworkList(0, SETUP_SCAN_MAX, raw);
        if (n < 0)
        {
            fatal("scan list", n);
        }
    }
    scan_policy(false);
    if (n > SETUP_SCAN_MAX)
    {
        n = SETUP_SCAN_MAX;
    }
    /* In place, front to back: each list entry is smaller than a raw one. */
    for (long i = 0; i < n; i++)
    {
        Sl_WlanNetworkEntry_t r = raw[i];
        scan_entry_t e;
        e.len = r.ssid_len;
        memcpy(e.ssid, r.ssid, 32);
        e.sec = r.sec_type;
        e.rssi = r.rssi;
        scan[i] = e;
    }
    scan_n = setup_scan_process(scan, (unsigned)n);
    debug_verbose("net: %ld networks found, %u offered\n", n, scan_n);
    open_ap();
}

static void leave_setup(void)
{
    sound_chime(CHIME_SIGNAL);
    stop_services_or_restart();
    set_policy(true);
    role_station_restart(R_ROLE, "leave setup");
    mode = M_STATION;
    ledui_request(LED_READY_IDLE);
    ledui_request(LED_CONFIRMATION);
}

/* A new search from setup mode, asked for by the setup page: station role
 * without auto-connect, then the setup search opens the access point again. */
static void rescan(void)
{
    debug_printf("net: setup rescan\n");
    set_policy(false);
    stop_services_or_restart();
    role_station_restart(R_ROLE, "rescan");
    setup_search();
}

/* The setup gesture and its order of checks. */
static void setup_gesture(void)
{
    if (going_down)
    {
        debug_printf("net: setup gesture off\n");
        return;
    }
    if (offline)
    {
        offline_off();
        return;
    }
    profile_pending = false;
    if (role == ROLE_AP)
    {
        leave_setup();
    }
    else
    {
        setup_search();
    }
}

/* --- join attempt --- */

/* An error indication: error pattern, then the sound prompt. */
static void error_indication(unsigned prompt)
{
    ledui_request(LED_ERROR);
    player_prompt(prompt, false);
}

static void join(uint8_t language)
{
    static uint8_t ssid[32], key[64];
    taskENTER_CRITICAL();
    uint8_t ssid_len = creds.ssid_len, key_len = creds.key_len, m = creds.mode;
    memcpy(ssid, creds.ssid, ssid_len);
    memcpy(key, creds.key, key_len);
    taskEXIT_CRITICAL();
    debug_verbose("net: join, SSID %u B, mode %u\n", ssid_len, m);
    /* 1 */
    if (language != settings_language() && settings_set_language)
    {
        settings_set_language(language);
    }
    /* 2 */
    sound_chime(CHIME_SIGNAL);
    ledui_request(LED_CONNECTING);
    mode = M_JOIN;
    join_handover = true;
    /* 3 */
    set_policy(false);
    join_conn = join_addr = false;
    join_waiting = true;
    /* 4 */
    stop_services_or_restart();
    role_station_restart(R_ROLE, "join");
    join_handover = false;
    /* 5 */
    SlSecParams_t sec = {setup_join_security(m), (int8_t *)key, key_len};
    if (sec.Type == SL_SEC_TYPE_OPEN)
    {
        sec.Key = NULL;
        sec.KeyLen = 0;
    }
    long r = sl_WlanConnect((const int8_t *)ssid, ssid_len, NULL, &sec, NULL);
    if (r < 0)
    {
        debug_printf("net: sl_WlanConnect %ld\n", r);
    }
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
        memcpy(prof_ssid, ssid, ssid_len);
        prof_ssid_len = ssid_len;
        memcpy(prof_key, key, key_len);
        prof_key_len = key_len;
        prof_sec = setup_profile_security(m);
        if (m > 3)
        {
            debug_verbose("net: security mode %u stored as open\n", m);
        }
        profile_pending = true;
        player_prompt(1, false);
        ledui_request(LED_READY_IDLE);
        ledui_request(LED_CONFIRMATION);
        cloud_sync_request(false);
        return;
    }
    debug_printf("net: join FAILED %s\n", conn ? "no address" : "not associated");
    stop_services_or_restart();
    if (set_policy(true))
    {
        role_station_restart(R_POLICY, "join failed");
    }
    ledui_request(LED_READY_IDLE);
    error_indication(conn ? 20 : 19);
}

/* --- profile storage --- */

static void store_profile(void)
{
    slot_info_t *s = work.prof.slots;
    int8_t *name = profile_name;
    for (int i = 0; i < 7; i++)
    {
        int16_t len = 0;
        uint8_t bssid[6] = {0};
        SlSecParams_t sec = {0};
        uint32_t prio;
        memset(&s[i], 0, sizeof s[i]);
        if (sl_WlanProfileGet(i, name, &len, bssid, &sec, NULL, &prio) < 0)
        {
            continue;
        }
        s[i].present = true;
        s[i].len = len < 0 || len > 255 ? 255 : (uint8_t)len;
        if (s[i].len <= 32)
        {
            memcpy(s[i].ssid, name, s[i].len);
        }
        s[i].sec = sec.Type;
        for (int k = 0; k < 6; k++)
        {
            s[i].bssid |= bssid[k] != 0;
        }
    }
    slot_plan_t p;
    setup_slot_plan(s, prof_ssid, prof_ssid_len, prof_sec, &p);
    for (unsigned i = 0; i < p.n_before; i++)
    {
        long r = sl_WlanProfileDel(p.del_before[i]);
        if (r < 0)
        {
            fatal("profile delete", r);
        }
    }
    SlSecParams_t sec = {prof_sec, (int8_t *)prof_key, prof_key_len};
    if (prof_sec == SL_SEC_TYPE_OPEN)
    {
        sec.Key = NULL;
        sec.KeyLen = 0;
    }
    long slot = sl_WlanProfileAdd((const int8_t *)prof_ssid, prof_ssid_len, NULL, &sec, NULL, 0, 0);
    if (slot < 0)
    {
        fatal("profile add", slot);
    }
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)slot);
    log_fn(LOG_INFO, 24, p.match ? 7184 : 7172, 0, &a);
    if (slot != p.expect)
    {
        debug_verbose("net: profile slot %ld, %d expected\n", slot, p.expect);
    }
    if (p.del_after >= 0 && slot != p.del_after)
    {
        long r = sl_WlanProfileDel(p.del_after);
        if (r < 0)
        {
            fatal("profile delete", r);
        }
        a.n = 0;
        log_arg_num(&a, p.del_after);
        log_fn(LOG_INFO, 24, 7178, 0, &a);
    }
    debug_printf("net: profile slot %ld, del %d\n", slot, p.del_after);
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
        debug_printf("net: profile drop, state %u\n", state);
        return;
    }
    switch (error)
    {
    case CE_OK:
        net_cloud_state(s, 6);
        store_profile();
        set_policy(true);
        stop_services_or_restart();
        break;
    case CE_NAME:
    case CE_CONNECT:
    case CE_TRANSFER:
    case CE_TIMEOUT:
        /* The hold of the announcement queue belongs to the
         * player; here the red blinking stays 15 s before the ready
         * display. */
        ledui_request(LED_NETWORK_ERROR);
        player_prompt(error == CE_NAME ? 21 : 23, false);
        ready_at = tick_ms() + 15000;
        if (!ready_at)
        {
            ready_at = 1;
        }
        break;
    default:
        debug_printf("net: profile drop %d\n", error);
        break;
    }
}

/* --- events --- */

static void handle(const event_t *e)
{
    switch (e->kind)
    {
    case EV_DISCONNECT:
        end_session();
        debug_printf("net: disconnect %u\n", e->len);
        if (role != ROLE_STA)
        {
            break;
        }
        set_state(0, "disconnection");
        if (e->len == 6)
        {
            power_policy();
        }
        break;
    case EV_CONNECT:
    {
        end_session();
        char ssid[33];
        memcpy(ssid, e->ssid, e->len);
        ssid[e->len] = 0;
        debug_printf("net: connected to \"%s\"\n", ssid);
        if (role == ROLE_STA && !offline)
        {
            set_state(3, "connection");
        }
        break;
    }
    case EV_LINK_FAULT:
        debug_printf("net: link fault\n");
        if (role != ROLE_STA || offline || join_waiting)
        {
            break; /* not while a join attempt waits */
        }
        log_fn(LOG_INFO, 24, 186, 0, NULL);
        link_fault_at = tick_ms();
        set_state(2, "link fault");
        break;
    case EV_ADDRESS:
    {
        uint32_t ip = e->ip, gw = e->gateway;
        debug_printf("net: address %lu.%lu.%lu.%lu, gateway %lu.%lu.%lu.%lu\n", (unsigned long)(ip >> 24),
                     (unsigned long)(ip >> 16 & 255), (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255),
                     (unsigned long)(gw >> 24), (unsigned long)(gw >> 16 & 255), (unsigned long)(gw >> 8 & 255),
                     (unsigned long)(gw & 255));
        end_session();
        if (role != ROLE_STA || offline || ip == gw || state > 3)
        {
            break;
        }
        session = ++session_count;
        set_state(4, "address");
        cloud_online_round(session);
        break;
    }
    case EV_ERROR:
        debug_printf("net: error 0x%04lx\n", (unsigned long)e->info);
        break;
    case EV_FAULT:
        debug_printf("net: drv fault %lu, 0x%lx%s\n", (unsigned long)e->ip, (unsigned long)e->info,
                     e->running ? "" : " (stopped)");
        if (!e->running)
        {
            break;
        }
        /* The procedure that stopped a network service reacts itself. */
        if (e->ip == SL_DEVICE_DRIVER_TIMEOUT_CMD_COMPLETE && e->info == OPCODE_NETAPP_STOP)
        {
            break;
        }
        restart_if_running(R_FAULT, "fault");
        break;
    case EV_SHUTDOWN:
        shutdown(R_POWER, "power");
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
        else
        {
            debug_printf("net: submit off, mode %u\n", mode);
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
    uint32_t seen_replaced = 0, seen_lost = 0, seen_logged = 0, seen_failed = 0;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(state == 2 || ready_at ? 100 : 1000));
        event_t e;
        /* the other requests first, then the latest network
         * event. */
        while (xQueueReceive(queue, &e, 0) == pdTRUE)
        {
            handle(&e);
        }
        taskENTER_CRITICAL();
        bool have = latest_set;
        if (have)
        {
            e = latest;
        }
        latest_set = false;
        uint32_t rep = replaced, lo = lost;
        taskEXIT_CRITICAL();
        if (rep != seen_replaced || lo != seen_lost)
        {
            debug_verbose("net: %lu events replaced, %lu lost\n", (unsigned long)(rep - seen_replaced),
                          (unsigned long)(lo - seen_lost));
            seen_replaced = rep;
            seen_lost = lo;
        }
        if (have && !going_down)
        {
            handle(&e);
        }
        if (state == 2 && tick_ms() - link_fault_at > 4000)
        {
            log_args_t a = {0};
            log_arg_num(&a, 2);
            log_fn(LOG_INFO, 24, 6160, 0, &a);
            set_state(0, "link fault end");
        }
        if (cloud_restart_wanted())
        {
            restart_if_running(R_BUFFERS, "no buffers");
        }
        /* the ready display 15 s after a failure indication. */
        if (ready_at && (int32_t)(tick_ms() - ready_at) >= 0)
        {
            ready_at = 0;
            ledui_request(LED_READY_IDLE);
        }
        uint32_t pl = post_logged, ff = form_failed;
        if (pl != seen_logged || ff != seen_failed)
        {
            debug_verbose("net: tokens ignored %lu, bad hex %lu\n", (unsigned long)pl,
                          (unsigned long)ff);
            seen_logged = pl;
            seen_failed = ff;
        }
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
        return true;
    }
    if (mode == M_SETUP)
    {
        debug_verbose("net: ear held in setup mode, ignored\n");
        return true;
    }
    return false;
}

void net_cloud_first_result(uint32_t s, int error)
{
    if (profile_pending)
    {
        post_request(EV_CLOUD_RESULT, s, (uint32_t)error);
    }
}

long net_profiles_delete(void)
{
    long r = sl_WlanProfileDel(255); /* all profiles */
    debug_printf("net: all profiles deleted: %ld\n", r);
    return r;
}

/* Network shutdown for the power-off and restart sequences: carried out by the
 * net activity; returns when the network processor is stopped. */
void net_shutdown(void)
{
    going_down = true;
    if (!net_task || xTaskGetCurrentTaskHandle() == net_task)
    {
        shutdown(R_POWER, "power");
        return;
    }
    event_t e = {0};
    e.kind = EV_SHUTDOWN;
    while (xQueueSend(queue, &e, pdMS_TO_TICKS(100)) != pdTRUE)
        ;
    xTaskNotifyGive(net_task);
    /* the caller is activity 1; the shutdown's own waits are
     * bounded (hand-over 54 s, stop 53 s). */
    do
    {
        monitor_feed();
    } while (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE);
}

void net_start(void)
{
    /* 640 words: the restart path holds a function record
     * and debug output on top of the driver calls. */
    static StackType_t stack[576]; /* worst case 1536 B (make stackcheck), +25 % */
    static StaticTask_t tcb;
    queue = xQueueCreateStatic(8, sizeof(event_t), queue_buf, &queue_mem);
    done = xSemaphoreCreateBinaryStatic(&done_mem);
    osi_spawn_task_start();
    net_task = xTaskCreateStatic(task, "net", 576, NULL, 3, stack, &tcb);
}
