/*
 * Polling round for figures: presence phase,
 * fresh read, probe phase with receiver sweep, the two remembered figures,
 * publication to the player. Of the log records only 8627 and 8630 are
 * sent (teddyCloud shows the figure from them); the others are on the
 * serial output so far.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "port.h"
#include "bootstate.h"
#include "debug.h"
#include "monitor.h"
#include "nfc.h"
#include "player.h"
#include "rtnl.h"
#include "tag.h"
#include "tick.h"

static const uint8_t sweep[4] = {0x22, 0x21, 0x23, 0x33};

/* records 8627 (published) and 8630 (removed), arguments 1-2
 * the UID as teddyCloud reads it: two big-endian halves, the low half
 * (printed bytes 5 to 8) first, then the high half beginning with 0xE0. */
static void uid_record(uint16_t function, const uint8_t u[8])
{
    log_args_t a = {0};
    log_arg_num(&a, (int32_t)((uint32_t)u[3] | (uint32_t)u[2] << 8 | (uint32_t)u[1] << 16 | (uint32_t)u[0] << 24));
    log_arg_num(&a, (int32_t)((uint32_t)u[7] | (uint32_t)u[6] << 8 | (uint32_t)u[5] << 16 | (uint32_t)u[4] << 24));
    log_fn(LOG_INFO, 15, function, 0, &a);
}

static bool confirmed;
static uint8_t uid[8];    /* the figure made available */
static uint8_t token[32]; /* its 32 data bytes: auth token */

/* the two most recently published figures, [0] the newest. */
static struct
{
    bool valid;
    uint8_t uid[8], token[32];
} mem[2];

/* record 8510 once per rejected UID until a fresh read ends
 * otherwise. */
static uint8_t rejected_uid[8];
static bool rejected_last;

/* F8: a tag the original firmware rejects or cannot unlock may still
 * be a foreign tag; `foreign` while one is made available. */
static bool foreign_candidate, foreign;
/* Result of the last failed unlock, printed once until it changes or the
 * tag is gone (a locked foreign tag fails in every round). */
static uint8_t last_fail;
static bool rf_seen;
static uint8_t rssi_max_main, rssi_max_aux;

static void print_uid(const char *what, const uint8_t u[8])
{
    debug_printf("tag: %s %02X%02X%02X%02X%02X%02X%02X%02X\n", what, u[7], u[6], u[5], u[4], u[3],
                 u[2], u[1], u[0]);
}

static void note_rssi(void)
{
    uint8_t r = nfc_rssi();
    uint8_t main = r & 7, aux = (r >> 3) & 7;
    if (main > rssi_max_main)
    {
        rssi_max_main = main;
    }
    if (aux > rssi_max_aux)
    {
        rssi_max_aux = aux;
    }
    if (main >= 2 || aux)
    {
        rf_seen = true;
    }
}

/* Diagnostic: the IRQ status values of the last attempt. */
static void print_irqs(const char *what)
{
    const uint8_t *irq;
    unsigned n = nfc_diag_irqs(&irq);
    debug_verbose("tag: %s, IRQ status %02x %02x %02x %02x %02x %02x (%u)\n", what, n > 0 ? irq[0] : 0xEE,
                  n > 1 ? irq[1] : 0xEE, n > 2 ? irq[2] : 0xEE, n > 3 ? irq[3] : 0xEE, n > 4 ? irq[4] : 0xEE,
                  n > 5 ? irq[5] : 0xEE, n);
}

/* Unlock; returns the result of SET PASSWORD (or of the step that
 * failed before it). */
static nfc_result_t leave_privacy(uint32_t password)
{
    uint16_t random;
    uint8_t seen[8];
    tick_wait(6);
    nfc_result_t r = nfc_random(NULL, 3, &random);
    if (r != NFC_OK)
    {
        return r;
    }
    tick_wait(6);
    if (nfc_inventory(seen) == NFC_OK)
    {
        print_uid("privacy not enabled (8646)", seen);
    }
    return nfc_set_password(NULL, random, password);
}

/* Returns true when the figure is published. */
static bool fresh_read(void)
{
    uint32_t t0 = tick_ms();
    bool rejected_before = rejected_last;
    rejected_last = false;
    foreign_candidate = false;
    /* The default password of the SLIX-L datasheet first,
     * the other one as fallback. */
    uint32_t password = NFC_PASSWORD_FACTORY;
    nfc_field_on(0x10);
    nfc_result_t r = leave_privacy(password);
    if (r == NFC_PASSWORD)
    {
        nfc_field_off();
        nfc_field_on(0x10);
        password = NFC_PASSWORD_BOXINE;
        r = leave_privacy(password);
        foreign_candidate = r == NFC_PASSWORD;
    }
    if (r != NFC_OK)
    {
        if (r != last_fail)
        {
            debug_printf("tag: privacy stays, %d%s\n", r, r == NFC_PASSWORD ? " (542)" : "");
        }
        last_fail = (uint8_t)r;
        nfc_field_off();
        return false;
    }
    last_fail = NFC_OK;
    debug_verbose("tag: privacy left with the %s password\n",
                  password == NFC_PASSWORD_FACTORY ? "datasheet default" : "fallback");

    uint8_t u[8];
    r = nfc_inventory(u);
    if (r != NFC_OK)
    {
        debug_printf("tag: inventory FAILED %d (544)\n", r);
        print_irqs("inventory");
        nfc_field_off();
        return false;
    }
    print_uid("UID", u);
    bool valid = u[5] == 0x03 && u[6] == 0x04 && u[7] == 0xE0;

    uint8_t data[32];
    bool blocks_ok = true;
    for (uint8_t b = 0; b < 8; b++)
    {
        r = nfc_read_block(u, b, data + 4 * b);
        if (r != NFC_OK)
        {
            debug_printf("tag: block %u FAILED %d (550)\n", b, r);
            print_irqs("block read");
            blocks_ok = false;
        }
    }
    uint8_t extra[4];
    bool block8 = nfc_read_block(u, 8, extra) == NFC_OK;

    /* Re-lock (nfc_enable_privacy) commented out: the tag stays without
     * privacy after a read. */
    // uint16_t random;
    // r = nfc_random(u, 3, &random);
    // if (r == NFC_OK)
    // {
    //     r = nfc_enable_privacy(u, random, password);
    // }
    // if (r != NFC_OK)
    // {
    //     debug_printf("tag: re-lock failed %d (548)\n", r);
    //     print_irqs("re-lock");
    // }
    nfc_field_off();

    if (blocks_ok)
    {
        for (int row = 0; row < 2; row++)
        {
            const uint8_t *d = data + 16 * row;
            debug_verbose("tag: data %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                          d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12],
                          d[13], d[14], d[15]);
        }
    }
    if (!valid || block8)
    {
        if (!rejected_before || memcmp(rejected_uid, u, 8))
        {
            debug_printf("tag: rejected, reason %u (8510)\n", !valid ? 8507u : 8508u);
        }
        memcpy(rejected_uid, u, 8);
        rejected_last = true;
        foreign_candidate = true;
        return false;
    }
    if (!blocks_ok)
    {
        return false;
    }
    memcpy(uid, u, 8);
    memcpy(token, data, 32);
    mem[1] = mem[0];
    mem[0].valid = true;
    memcpy(mem[0].uid, u, 8);
    memcpy(mem[0].token, data, 32);
    debug_printf("tag: fresh ok %lu ms (8719, 8627)\n", (unsigned long)(tick_ms() - t0));
    uid_record(8627, uid);
    player_placed(uid, token);
    debug_verbose("tag: free stack %u words\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return true;
}

/* one INVENTORY in its own field-on period, nothing else sent
 * to the tag. */
static bool foreign_inventory(uint8_t u[8])
{
    nfc_field_on(0x10);
    tick_wait(6);
    nfc_result_t r = nfc_inventory(u);
    nfc_field_off();
    return r == NFC_OK;
}

/* One addressed probe in its own field-on period. */
static bool probe_alone(const uint8_t *u, uint8_t setting, unsigned retries)
{
    uint16_t random;
    nfc_field_on(setting);
    nfc_result_t r = nfc_random(u, retries, &random);
    if (r != NFC_OK && setting != 0x10)
    {
        note_rssi();
    }
    if (r == NFC_COLLISION)
    {
        rf_seen = true;
    }
    nfc_field_off();
    return r == NFC_OK;
}

/* Presence probe; returns the next delay, 0 when the figure is gone. */
static uint32_t probe_phase(unsigned *attempts)
{
    uint16_t random;
    nfc_field_on(0x10);
    for (int i = 0; i < 3; i++)
    {
        ++*attempts;
        nfc_result_t r = nfc_random(uid, 3, &random);
        if (r == NFC_OK)
        {
            nfc_field_off();
            return 200;
        }
        if (r == NFC_COLLISION)
        {
            rf_seen = true;
        }
    }
    note_rssi();
    nfc_field_off();
    for (int pass = 0; pass < 4; pass++)
    {
        unsigned n = (pass == 0 || rf_seen) ? 4 : 2;
        for (unsigned s = 0; s < n; s++)
        {
            ++*attempts;
            if (probe_alone(uid, sweep[s], 1))
            {
                return rf_seen ? 20 : 200;
            }
        }
    }
    return 0;
}

static void tag_task(void *arg)
{
    (void)arg;
    uint32_t delay = 200;
    bool fast = false;
    debug_verbose("tag: polling\n");
    /* First reader access 200 ms after the polling starts. */
    tick_wait(200);
    nfc_lock();
    nfc_reinit();
    bool st = nfc_selftest();
    bootstate_report(BOOT_CHECK_READER, st);
    debug_printf("tag: reader SPI %s\n", st ? "ok" : "FAILED");
    nfc_unlock();
    int last_presence = -1;

    for (;;)
    {
        monitor_mark(ACT_TAG);
        tick_wait(delay);
        delay = 200;
        nfc_lock();
        if (nfc_reinit_pending())
        {
            nfc_reinit();
        }
        rf_seen = false;
        rssi_max_main = rssi_max_aux = 0;
        unsigned attempts = 0;

        if (foreign)
        {
            /* Present only while the INVENTORY returns the same UID. */
            uint8_t u[8];
            if (!foreign_inventory(u) || memcmp(u, uid, 8))
            {
                foreign = false;
                print_uid("foreign tag removed (8630)", uid);
                player_removed();
            }
        }
        else if (confirmed)
        {
            delay = probe_phase(&attempts);
            if (!delay)
            {
                confirmed = false;
                delay = 200;
                print_uid("removed (8630)", uid);
                uid_record(8630, uid);
                player_removed();
            }
        }
        else
        {
            uint16_t random;
            nfc_field_on(0x10);
            nfc_result_t r = nfc_random(NULL, 3, &random);
            nfc_field_off();
            if ((r == NFC_OK) != (last_presence == NFC_OK))
            {
                last_presence = (int)r;
                debug_verbose("tag: presence probe result %d\n", r);
            }

            foreign_candidate = false;
            if (r != NFC_OK)
            {
                last_fail = NFC_OK;
            }
            if (r == NFC_OK)
            {
                /* the newest remembered figure first. */
                int k = -1;
                for (int i = 0; i < 2 && k < 0; i++)
                {
                    if (mem[i].valid && probe_alone(mem[i].uid, 0x10, 3))
                    {
                        k = i;
                    }
                }
                if (k >= 0)
                {
                    confirmed = true;
                    memcpy(uid, mem[k].uid, 8);
                    memcpy(token, mem[k].token, 32);
                    debug_printf("tag: remembered %d (8718)\n", k + 1);
                    print_uid("published (8627)", uid);
                    uid_record(8627, uid);
                    player_placed(uid, token);
                }
                else
                {
                    confirmed = fresh_read();
                }
            }
            /* no tag found, or one rejected or not unlocked. */
            uint8_t u[8];
            if (!confirmed && (r != NFC_OK || foreign_candidate) && foreign_inventory(u))
            {
                foreign = true;
                memcpy(uid, u, 8);
                memset(token, 0, sizeof token);
                print_uid("foreign tag placed (F8)", uid);
                player_placed(uid, token);
            }
        }
        nfc_unlock();
        if ((delay == 20) != fast)
        {
            fast = delay == 20;
            if (fast)
            {
                debug_verbose("tag: fast, RSSI %u/%u, %u tries (7523)\n", rssi_max_main,
                              rssi_max_aux, attempts);
            }
            else
            {
                debug_verbose("tag: fast ended (7524)\n");
            }
        }
    }
}

void tag_start(void)
{
    static StackType_t stack[TASK_STACK(352)]; /* worst case 800 B (make stackcheck), +25 % */
    static StaticTask_t tcb;
    /* Above the player, sound, LED and input activities (priorities 3 and
     * 4): the transmit FIFO refill must not wait
     * behind the decoder; below the SimpleLink spawn activity. */
    xTaskCreateStatic(tag_task, "tag", TASK_STACK(352), NULL, 5, stack, &tcb);
}
