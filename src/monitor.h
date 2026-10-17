/*
 * Monitoring of the activities and the watchdog feed.
 *
 * The monitored activities (3 cloud requests, 4 SD card and content,
 * 5 inputs, 6 tag reading, 7 log upload) mark progress at least once per
 * second, also when idle; an activity is monitored from its first mark.
 * Activity 1 (operating functions) runs monitor_round() once per second;
 * it feeds the watchdog at its end. A stalled activity does not stop the
 * feed: it is reported, and after 600 s of stalls the box panics. Only a
 * blocked activity 1 lets the watchdog expire.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>
#include <stdint.h>
#include "panic.h"

/* Progress mark of a monitored activity. */
void monitor_mark(activity_t a);
/* Activity 3 in its update round: limit 40000 ms instead of 20000 ms. */
void monitor_update_round(bool on);
/* The update round is running (it holds the idle power-off). */
bool monitor_in_update_round(void);
/* The box activity was set: busy is true while playing
 * content, playing a sound prompt, synchronising, preparing power-off or
 * restarting after an interrupted power-off. Each source keeps its own
 * flag; the box is busy while any of them is set. */
void monitor_box_activity(bool busy);   /* player: content, prompt */
void monitor_sync_activity(bool busy);  /* cloud: synchronising */
void monitor_power_activity(bool busy); /* power.c: preparing off, restarting */
/* An SD card transfer starts (sd.c). */
void monitor_sd_transfer(void);

/* The monitoring round of activity 1, at least once per second: stalls,
 * stall panic, busy state, stack reserves, then the watchdog feed. Does
 * nothing while a fault is handled. */
void monitor_round(void);
/* The extra feeds of activity 1 (before waiting for the flash
 * access lock, network shutdown, factory reset steps, power actions). */
void monitor_feed(void);
/* Code position of the last monitoring round (watchdog record). */
uint16_t monitor_op_position(void);

#endif
