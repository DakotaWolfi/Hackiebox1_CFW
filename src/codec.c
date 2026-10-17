/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "codec.h"
#include "debug.h"
#include "i2c.h"
#include "recovery.h"
#include "tick.h"

#define ADDR 0x18
#define PAGE_UNKNOWN 0xFF

static uint8_t page = PAGE_UNKNOWN;
static bool lost; /* a bus recovery reset the codec */

static bool select_page(uint8_t p)
{
    if (page == p)
    {
        return true;
    }
    if (i2c_write_reg(ADDR, 0, p) != I2C_OK)
    {
        page = PAGE_UNKNOWN;
        return false;
    }
    page = p;
    return true;
}

static void recover(void)
{
    bus_recovery();
    lost = true;
}

bool codec_lost(void)
{
    bool l = lost;
    lost = false;
    return l;
}

bool codec_write(uint16_t page_reg, uint8_t value)
{
    uint8_t p = page_reg >> 8, reg = page_reg & 0xFF;

    /* A failed access is followed by a bus recovery and one retry; a
     * second failure is fatal (the error handling follows). */
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (select_page(p) && i2c_write_reg(ADDR, reg, value) == I2C_OK)
        {
            return true;
        }
        debug_printf("codec write %u/0x%02x failed%s\n", p, reg, attempt ? " again (fatal)" : "");
        page = PAGE_UNKNOWN;
        if (!attempt)
        {
            recover();
        }
    }
    return false;
}

bool codec_read(uint8_t reg, uint8_t *value)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (select_page(0) && i2c_read_regs(ADDR, reg, value, 1) == I2C_OK)
        {
            return true;
        }
        debug_printf("codec read 0x%02x failed%s\n", reg, attempt ? " again (fatal)" : "");
        page = PAGE_UNKNOWN;
        if (!attempt)
        {
            recover();
        }
    }
    return false;
}

typedef struct
{
    uint16_t reg;
    uint8_t value;
} reg_value_t;

/* Configuration steps 1 to 10, then 11 to 21 after a 10 ms wait. */
static const reg_value_t config_a[] = {
    {CODEC_REG(0, 0x01), 0x01},
    {CODEC_REG(0, 0x04), 0x07},
    {CODEC_REG(0, 0x06), 0x20},
    {CODEC_REG(0, 0x07), 0x00},
    {CODEC_REG(0, 0x08), 0x00},
    {CODEC_REG(0, 0x05), 0x96},
    {CODEC_REG(0, 0x0B), 0x84},
    {CODEC_REG(0, 0x0C), 0x86},
    {CODEC_REG(0, 0x0D), 0x01},
    {CODEC_REG(0, 0x0E), 0x00},
};
static const reg_value_t config_b[] = {
    {CODEC_REG(0, 0x1B), 0x00},
    {CODEC_REG(0, 0x3C), 0x19},
    {CODEC_REG(1, 0x21), 0x4E},
    {CODEC_REG(1, 0x22), 0x70},
    {CODEC_REG(1, 0x23), 0x44},
    {CODEC_REG(1, 0x2E), 0x0B},
    {CODEC_REG(1, 0x2C), 0xE0},
    {CODEC_REG(3, 0x10), 0x01},
    {CODEC_REG(0, 0x43), 0x8C},
    {CODEC_REG(0, 0x30), 0x80},
    {CODEC_REG(0, 0x33), 0x14},
};

static bool write_list(const reg_value_t *list, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
    {
        if (!codec_write(list[i].reg, list[i].value))
        {
            return false;
        }
    }
    return true;
}

/* A bus recovery during the configuration restarts it from step 1. */
bool codec_configure(bool *headset)
{
    for (int run = 0; run < 3; run++)
    {
        lost = false;
        page = PAGE_UNKNOWN;
        bool ok = write_list(config_a, sizeof config_a / sizeof config_a[0]);
        if (ok)
        {
            tick_wait(10);
            ok = write_list(config_b, sizeof config_b / sizeof config_b[0]);
        }
        if (ok)
        {
            tick_wait(200);
            ok = codec_headset_inserted(headset);
        }
        if (!lost)
        {
            return ok;
        }
        debug_printf("codec: again after recovery\n");
    }
    return false;
}

void codec_path_writes(codec_path_t path)
{
    /* Common teardown. */
    codec_write(CODEC_REG(1, 0x24), 0x7F);
    codec_write(CODEC_REG(1, 0x25), 0x7F);
    codec_write(CODEC_REG(1, 0x26), 0x7F);
    tick_wait(50);
    codec_write(CODEC_REG(1, 0x28), 0x00);
    codec_write(CODEC_REG(1, 0x29), 0x00);
    codec_write(CODEC_REG(1, 0x2A), 0x00);
    tick_wait(50);
    codec_write(CODEC_REG(1, 0x1F), 0x00);
    codec_write(CODEC_REG(1, 0x20), 0x00);
    tick_wait(50);

    if (path == PATH_SPEAKER)
    {
        codec_write(CODEC_REG(1, 0x2A), 0x04);
        codec_write(CODEC_REG(1, 0x20), 0x86);
        codec_write(CODEC_REG(1, 0x26), 0x92);
        tick_wait(50);
        codec_write(CODEC_REG(0, 0x3F), 0xFD);
    }
    else if (path == PATH_HEADPHONES)
    {
        codec_write(CODEC_REG(1, 0x28), 0x06);
        codec_write(CODEC_REG(1, 0x29), 0x06);
        codec_write(CODEC_REG(1, 0x1F), 0xC2);
        codec_write(CODEC_REG(1, 0x24), 0x92);
        codec_write(CODEC_REG(1, 0x25), 0x92);
        tick_wait(50);
        codec_write(CODEC_REG(0, 0x3F), 0xD5);
    }
}

void codec_set_gain_db(int gain_db)
{
    uint8_t v = (uint8_t)(int8_t)(2 * gain_db);

    codec_write(CODEC_REG(0, 65), v);
    codec_write(CODEC_REG(0, 66), v);
    for (int i = 0; i < 20; i++)
    {
        uint8_t flags;
        if (codec_read(38, &flags) && (flags & 0x11) == 0x11)
        {
            return;
        }
        tick_wait(10);
    }
}

void codec_dac_mute(bool mute)
{
    codec_write(CODEC_REG(0, 64), mute ? 0x0C : 0x00);
}

bool codec_headset_inserted(bool *inserted)
{
    uint8_t v;

    if (!codec_read(46, &v))
    {
        return false;
    }
    *inserted = (v & 0x10) != 0;
    return true;
}
