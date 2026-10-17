/*
 * F12 remote commands: the actions
 * behind the commands 64 to 73 and the status snapshot. The frames, the
 * acknowledgement and the records are the log activity's (rtnl.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef REMOTE_H
#define REMOTE_H

#include <stdbool.h>
#include <stdint.h>

/* The values of the status record. */
typedef struct
{
    uint32_t state; /* 0 idle without figure, 1 idle with figure, 2 playing, 3 paused, 4 preparing */
    uint8_t uid[8]; /* B0..B7, zero without one */
    uint32_t audio_id, chapter, position;
    uint32_t volume;  /* bits 0-7 step, 8-15 limit, bit 16 headphones */
    uint32_t battery; /* mV, bit 31 on the station */
    uint32_t locks;   /* bit 0 slap, 1 tilt, 2 volume */
} remote_status_t;

/* Carries out remote command c (64 to 73) with its arguments; the result:
 * 0 done, 1 not possible now, 2 invalid arguments. */
uint8_t remote_command(uint8_t c, const uint8_t *arg, unsigned n);

/* The snapshot for a status record about to be sent (remembered as the
 * last one sent). */
void remote_status(remote_status_t *s);

/* State, UID, chapter or volume differ from the last record sent. */
bool remote_changed(void);

#endif
