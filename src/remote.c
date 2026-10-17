/*
 * F12 remote commands (see remote.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "battery.h"
#include "board.h"
#include "ext.h"
#include "player.h"
#include "power.h"
#include "remote.h"

/* Provided by their modules (weak, NULL when absent); until then command
 * 73 has result 1. */
int cloud_remote_update_check(void) __attribute__((weak)); /* command 73 */
/* The last measurement (battery.c), so that the status record does not
 * read the ADC FIFO beside the power activity; until it exists, a
 * measurement of its own. */
uint32_t battery_last_mv(void) __attribute__((weak));

static struct
{
    uint8_t state, uid[8], step, limit, headphones;
    uint32_t chapter;
} sent;

uint8_t remote_command(uint8_t c, const uint8_t *arg, unsigned n)
{
    if (c != 72)
    {
        power_activity();
    }
    switch (c)
    {
    case 64:
        if (n <= 8)
        {
            return PLAYER_RC_ARGS;
        }
        return ext_store(arg, n) ? PLAYER_RC_DONE : PLAYER_RC_STATE;
    case 65:
        return (uint8_t)player_remote_pause();
    case 66:
        return (uint8_t)player_remote_continue();
    case 67:
        return (uint8_t)player_remote_stop();
    case 68:
    case 69:
        return (uint8_t)player_remote_chapter(c == 68);
    case 70:
        if (n < 1 || arg[0] > 15)
        {
            return PLAYER_RC_ARGS;
        }
        return (uint8_t)player_remote_volume(arg[0]);
    case 71:
        if (n < 8)
        {
            return PLAYER_RC_ARGS;
        }
        return (uint8_t)player_remote_play(arg);
    case 72:
        return PLAYER_RC_DONE;
    case 73:
        return cloud_remote_update_check ? (uint8_t)cloud_remote_update_check() : PLAYER_RC_STATE;
    default:
        return PLAYER_RC_STATE;
    }
}

void remote_status(remote_status_t *s)
{
    player_status_t p;
    player_status(&p);
    memset(s, 0, sizeof *s);
    s->state = p.state;
    memcpy(s->uid, p.uid, 8);
    s->audio_id = p.audio_id;
    s->chapter = p.chapter;
    s->position = p.position;
    s->volume = p.step | (uint32_t)p.limit << 8 | (uint32_t)(p.headphones & 1) << 16;
    s->battery = (battery_last_mv ? battery_last_mv() : battery_measure()) | (board_on_station() ? 0x80000000u : 0);
    s->locks = p.locks & 7;
    sent.state = p.state;
    memcpy(sent.uid, p.uid, 8);
    sent.chapter = p.chapter;
    sent.step = p.step;
    sent.limit = p.limit;
    sent.headphones = p.headphones;
}

bool remote_changed(void)
{
    player_status_t p;
    player_status(&p);
    return p.state != sent.state || memcmp(p.uid, sent.uid, 8) || p.chapter != sent.chapter || p.step != sent.step ||
           p.limit != sent.limit || p.headphones != sent.headphones;
}
