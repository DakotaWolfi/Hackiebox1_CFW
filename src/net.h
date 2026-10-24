/*
 * Network processor and Wi-Fi in the station role (the setup-mode data
 * rules are in setup.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

void net_start(void);

/* Connection state (0 to 6). */
uint8_t net_state(void);

/* Number of the current network session, 0 while there is none. */
uint32_t net_session(void);

/* A change of the connection state requested by the cloud for session s. */
void net_cloud_state(uint32_t s, uint8_t state);

/* Network shutdown (power-off, restart): log drain,
 * hand-over, stop and power-down of the network processor. Blocks until
 * done (up to about 2 min in the worst case); afterwards the network stays
 * down. Not from an interrupt handler. */
void net_shutdown(void);

/* True during a network shutdown until the stop command has been sent:
 * slap and tilt inputs are discarded. */
bool net_handing_over(void);

/* Setup gesture: queued to the net
 * activity, which switches offline mode off, leaves setup mode or starts
 * the setup search. The caller drops it while shutting down. */
void net_setup_request(void);

/* The setup page asks for a new search: in setup mode the net activity
 * closes the access point, searches again and opens it again. */
void net_setup_rescan(void);

/* An ear held alone for 3 s: true when the net activity takes
 * it (setup search: offline mode on; setup mode: ignored); false: a sync
 * request for the player. */
bool net_sync_gesture(void);

/* Offline flag: the player answers sync requests and failed
 * cloud operations with prompt 10; saved with the retained
 * state and restored before net_start after a wake-up from the off state. */
bool net_offline(void);
void net_offline_restore(bool on);

/* Box activity "setup search" or "setup mode": no playback of figures,
 * no idle power-off. */
bool net_setup_active(void);

/* Result of the first cloud exchange of network session s (the online
 * round's time request), as cloud_error_t: completes or
 * drops a pending profile storage. */
void net_cloud_first_result(uint32_t s, int error);

/* Setup page: deletes every stored Wi-Fi profile; the
 * result of sl_WlanProfileDel, negative on failure. */
long net_profiles_delete(void);

/* Network processor version number, 0 until known. */
uint32_t net_sp_version(void);

#ifdef CFW_ESP32
/* starts the time synchronisation with the time server
 * (a name or an IP address) once in a start; later calls do nothing. */
void net_time_sync(const char *server);
#endif

/* --- setup page, served by the cloud activity (page.c) --- */

typedef struct scan_entry scan_entry_t;

/* One POST field of the setup protocol, as
 * from the token events. */
void net_setup_field(const uint8_t *name, unsigned name_len, const uint8_t *value, unsigned value_len);

/* The processed scan list offered in setup mode; the number
 * of entries, 0 outside setup mode. Fixed while the access point is open. */
unsigned net_scan_list(const scan_entry_t **list);

/* The page server may start (true: it is active until net_page_end); it
 * serves while net_page_on() is true. took: it stopped the network
 * processor's HTTP server. Before stopping the setup-mode services and
 * before a network shutdown the net activity clears net_page_on() and
 * waits until the server has ended. */
bool net_page_begin(void);
bool net_page_on(void);
void net_page_end(bool took);

#endif
