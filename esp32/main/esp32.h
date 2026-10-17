/*
 * Functions of the ESP32 platform part that the shared headers do not
 * declare (esp32/main only).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ESP32_H
#define ESP32_H

#include <stdbool.h>
#include <stdint.h>

/* ADC1 channels: GPIO8 the charger input, GPIO9 the battery. */
#define BOARD_ADC_CHARGER 7
#define BOARD_ADC_BATTERY 8

/* board.c */
int board_adc_mv(int ch);
bool board_wake_line(void);
void board_release_for_reset(void);

/* led.c: the LED channels are set up. */
bool led_ready(void);

/* battery.c */
void esp32_adc_calibration_init(void);
int esp32_adc_raw_to_mv(int raw);

/* assets.c: the assets partition, FatFs drive "1:". Names
 * are "CFW/<8.3 name>" (read and written) and "FW/SFX.BIN" (read only),
 * matched without regard to case. */
bool assets_mount(void);
bool assets_wl_acceptable(uint32_t part_size);
long assets_size(const char *name);
long assets_read(const char *name, uint32_t offset, void *buf, unsigned n);
long assets_replace(const char *name, const void *buf, unsigned n);
long assets_delete(const char *name);

/* nvsfile.c: small files in the NVS namespace "cfw". */
bool nvsfile_init(void);
long nvsfile_size(const char *key);
long nvsfile_read(const char *key, uint32_t offset, void *buf, unsigned n);
long nvsfile_write(const char *key, const void *buf, unsigned n);
long nvsfile_delete(const char *key);

/* slots.c: app slots, trial start, rollback. */
/* First of all, before the namespace is used: counts the start in RTC
 * memory and, at the third start of an unconfirmed image that ended before
 * slots_start(), goes back to the original firmware. */
void slots_early(void);
void slots_start(void);
/* The app slot running (0 to 2) and the original slot (0 to 2, 3 = none). */
unsigned slots_running(void);
unsigned slots_original(void);

/* wifi profiles of the original firmware, in net.c */
void net_import_original_profiles(void);

/* setup page built into the image (page_default.c): written to the card
 * when /cfw/setup.htm is missing. */
void page_default_install(void);

/* server block, in srvblock.c */
void srvblock_apply(void);

#endif
