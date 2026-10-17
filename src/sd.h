/*
 * SD card over the CC3200 SD host in 1-bit mode.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SD_H
#define SD_H

#include <stdbool.h>
#include <stdint.h>

/* SD host initialisation, start-up step 10 (24 MHz). */
void sd_host_init(void);
/* Diagnostics: pull-ups on CMD and DAT0, card clock. */
void sd_host_setup(bool pull_up, uint32_t card_clock);
uint32_t sd_host_clock(void);

/* Card identification. */
bool sd_identify(void);
/* Start-up identification with bus recoveries for up to 20 s. */
bool sd_start(void);
/* Bus recovery. */
bool sd_recover(void);
bool sd_ready(void);
uint32_t sd_sectors(void);

/* Whole sectors with CMD18 / CMD25 and CMD12, with the bus
 * recovery and one repetition. False only after a second
 * failure in the data area; the other failures panic. */
bool sd_read(uint32_t sector, uint8_t *buf, uint32_t count);
bool sd_write(uint32_t sector, const uint8_t *buf, uint32_t count);
/* First data sector of the mounted volume: a second failure before it is
 * a panic. Set by the mount (ff.c). */
void sd_set_system_end(uint32_t first_data_sector);
/* Erase of released sectors. */
void sd_erase(uint32_t first, uint32_t last);

#endif
