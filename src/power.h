/*
 * Battery rules and charging-station handling.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

/* First measurement when the main operation starts; `task`
 * is the activity that calls power_check() and is woken for the station
 * handling. */
void power_start(void *task);
/* The checks, about once per second and when woken. */
void power_check(void);
/* A debounced change of the station input. */
void power_station_event(bool on);
/* Resets the idle timer; power_check() also resets it when
 * the box activity becomes idle. */
void power_activity(void);
/* An ear press (input.c, instead of power_activity()): resets the idle
 * timer and, during the idle power-off, asks for a restart. */
void power_ear_press(void);
/* The box activity is shutting down: the input evaluation
 * discards everything but ear presses and docking. */
bool power_shutting_down(void);
/* Requests the idle power-off without the idle timer: it
 * starts at the next check that finds the box idle and not in its update
 * round, also with a figure on the box. */
void power_off_soon(void);
/* The idle power-off time off and on the charging station in minutes;
 * 0 = 10 minutes. */
void power_idle_minutes(uint16_t battery, uint16_t station);
/* The power-off of a critical battery is queued: it follows
 * sound prompt 9, before the figure on the box is evaluated again, so no
 * content starts meanwhile. */
bool power_critical_pending(void);

/* After a hibernation exit (or the off state without hibernation): the
 * retained state back into settings, volumes, offline mode and resume
 * record. In main() after sound_init() and player_init(),
 * before the scheduler starts. */
void power_restore(void);
/* Wake-up check, right after power_restore(); may go off again. Only with
 * CFW_HIBERNATE (without it the off state ends by an ear press or docking
 * only). */
void power_wake_check(void);

/* Hooks the idle power-off and the retained state use; weak defaults in
 * power.c until the modules provide them. */
bool player_idle(void);                 /* box activity idle: no content, prompt or sync */
void player_power_off(void);            /* power-off step 3: stop playback, drop requests, SD housekeeping; returns when done */
void sound_volume_get(uint8_t step[2]); /* speaker, headphone steps 0..15 */
void sound_volume_set(const uint8_t step[2]);
void player_timer_steps(uint8_t step[2]); /* the steps from before a running sleep timer replace these */
uint32_t player_resume_get(void);         /* pwrstate_resume() of the resume record, 0: none */
void player_resume_set(uint32_t r);       /* the kept 12-bit tag and half block after a wake-up */
bool net_offline(void);                   /* offline mode */
void net_offline_set(bool on);

#endif
