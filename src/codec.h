/*
 * TLV320DAC3100 audio codec: register access, start-up configuration,
 * output paths, gain and mute.
 *
 * Not thread-safe: only the sound activity calls these.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CODEC_H
#define CODEC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PATH_PARKED = 0,
    PATH_SPEAKER,
    PATH_HEADPHONES,
} codec_path_t;

/* Page and register of one access. */
#define CODEC_REG(page, reg) (((page) << 8) | (reg))

bool codec_write(uint16_t page_reg, uint8_t value);
/* Page 0 registers 38, 46 and 71 only. */
bool codec_read(uint8_t reg, uint8_t *value);

/* Start-up configuration up to the headset read; returns the headset state of step 22. */
bool codec_configure(bool *headset);

/* True once after a bus recovery reset the codec outside codec_configure;
 * the caller configures it again. */
bool codec_lost(void);

/* Common teardown and the writes of the new path. */
void codec_path_writes(codec_path_t path);

/* Writes 2 x gain to registers 65 and 66 and waits for it to settle. */
void codec_set_gain_db(int gain_db);

/* Register 64: 0x0C muted, 0x00 unmuted (without the gain
 * steps around it). */
void codec_dac_mute(bool mute);

/* Bit 4 of page 0 register 46. */
bool codec_headset_inserted(bool *inserted);

#endif
