/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include "accel.h"
#include "debug.h"
#include "i2c.h"

#define MMA8451Q_ADDR 0x1D
#define MMA8451Q_WHO_AM_I 0x0D
#define MMA8451Q_ID 0x1A
#define LIS3DH_ADDR 0x19
#define LIS3DH_WHO_AM_I 0x0F
#define LIS3DH_ID 0x33

accel_type_t accel_identify(void)
{
    uint8_t id;

    if (i2c_read_regs(MMA8451Q_ADDR, MMA8451Q_WHO_AM_I, &id, 1) == I2C_OK && id == MMA8451Q_ID)
    {
        return ACCEL_MMA8451Q;
    }
    if (i2c_read_regs(LIS3DH_ADDR, LIS3DH_WHO_AM_I, &id, 1) == I2C_OK && id == LIS3DH_ID)
    {
        return ACCEL_LIS3DH;
    }
    return ACCEL_NONE;
}

const char *accel_name(accel_type_t t)
{
    switch (t)
    {
    case ACCEL_MMA8451Q:
        return "MMA8451Q";
    case ACCEL_LIS3DH:
        return "LIS3DH";
    default:
        return "none";
    }
}

/* --- configuration and sample store --- */

#define STORE 32

static accel_type_t sensor, configured_type;
static int16_t store[STORE][3];
static unsigned stored, next;

static const uint8_t lis3dh_cfg[][2] = {
    {0x20, 0x77},
    {0x22, 0x80},
    {0x23, 0xA8},
    {0x24, 0x48},
    {0x38, 0x14},
    {0x3A, 0xE4},
    {0x3B, 0x50},
    {0x3C, 0xFF},
    {0x2E, 0x80},
};

static const uint8_t mma_cfg[][2] = {
    {0x2A, 0x02},
    {0x0E, 0x02},
    {0x2B, 0x00},
    {0x09, 0xD0},
    {0x0A, 0x08},
    {0x21, 0x54},
    {0x23, 0x1B},
    {0x24, 0x3F},
    {0x25, 0x3F},
    {0x26, 0x28},
    {0x27, 0x7F},
    {0x0F, 0x10},
    {0x2C, 0x12},
    {0x2D, 0x40},
    {0x2E, 0x40},
    {0x2A, 0x03},
};

bool accel_configure(accel_type_t t)
{
    uint8_t v;
    bool ok = true;
    sensor = ACCEL_NONE;
    stored = next = 0;
    if (t == ACCEL_LIS3DH)
    {
        for (unsigned i = 0; i < sizeof lis3dh_cfg / sizeof lis3dh_cfg[0]; i++)
        {
            ok &= i2c_write_reg(LIS3DH_ADDR, lis3dh_cfg[i][0], lis3dh_cfg[i][1]) == I2C_OK;
        }
    }
    else if (t == ACCEL_MMA8451Q)
    {
        i2c_read_regs(MMA8451Q_ADDR, 0x0C, &v, 1);
        i2c_read_regs(MMA8451Q_ADDR, 0x16, &v, 1);
        for (unsigned i = 0; i < sizeof mma_cfg / sizeof mma_cfg[0]; i++)
        {
            ok &= i2c_write_reg(MMA8451Q_ADDR, mma_cfg[i][0], mma_cfg[i][1]) == I2C_OK;
        }
        i2c_read_regs(MMA8451Q_ADDR, 0x0C, &v, 1);
        i2c_read_regs(MMA8451Q_ADDR, 0x16, &v, 1);
    }
    else
    {
        ok = false;
    }
    /* Readings start only after a complete configuration. */
    if (ok)
    {
        sensor = configured_type = t;
    }
    return ok;
}

void accel_restore(void)
{
    accel_type_t t = accel_identify();
    debug_printf("accel: recovered %s\n", accel_name(t));
    /* A part identified earlier is kept when none answers now. */
    if (t == ACCEL_NONE)
    {
        t = configured_type;
    }
    if (t != ACCEL_NONE && !accel_configure(t))
    {
        debug_printf("accel: recovery cfg FAILED\n");
    }
}

bool accel_active(void)
{
    return sensor != ACCEL_NONE;
}

accel_type_t accel_sensor(void)
{
    return sensor;
}

static void keep(int16_t x, int16_t y, int16_t z)
{
    store[next][0] = x;
    store[next][1] = y;
    store[next][2] = z;
    next = (next + 1) % STORE;
    if (stored < STORE)
    {
        stored++;
    }
}

/* Sensor read: returns the number of new samples, -1 on a bus failure;
 * *overflow gets the MMA8451Q overflow bit. */
static int sensor_read(bool *overflow)
{
    static uint8_t buf[STORE * 6];
    uint8_t status;
    unsigned n;
    *overflow = false;
    if (sensor == ACCEL_NONE)
    {
        return -1;
    }
    if (sensor == ACCEL_LIS3DH)
    {
        if (i2c_read_regs(LIS3DH_ADDR, 0x2F, &status, 1) != I2C_OK)
        {
            return -1;
        }
        n = (status & 0x40) ? 32 : (status & 0x1F);
        if (n && i2c_read_regs(LIS3DH_ADDR, 0x28 | 0x80, buf, (uint8_t)(n * 6)) != I2C_OK)
        {
            stored = next = 0;
            return -1;
        }
        for (unsigned i = 0; i < n; i++)
        {
            const uint8_t *s = buf + 6 * i;
            keep((int16_t)(s[0] | s[1] << 8) / 4, (int16_t)(s[2] | s[3] << 8) / 4,
                 (int16_t)(s[4] | s[5] << 8) / 4);
        }
    }
    else
    {
        if (i2c_read_regs(MMA8451Q_ADDR, 0x00, &status, 1) != I2C_OK)
        {
            return -1;
        }
        *overflow = (status & 0x80) != 0;
        n = status & 0x3F;
        if (n > 32)
        {
            debug_printf("accel: MMA8451Q %u samples, fatal\n", n);
            n = 32;
        }
        if (n && i2c_read_regs(MMA8451Q_ADDR, 0x01 | 0x80, buf, (uint8_t)(n * 3)) != I2C_OK)
        {
            stored = next = 0;
            return -1;
        }
        for (unsigned i = 0; i < n; i++)
        {
            keep((int8_t)buf[3 * i], (int8_t)buf[3 * i + 1], (int8_t)buf[3 * i + 2]);
        }
    }
    return (int)n;
}

bool accel_orientation(int16_t xyz[3])
{
    bool overflow;
    int n = sensor_read(&overflow);
    if (n < 0 || stored == 0 || (sensor == ACCEL_LIS3DH && n == 0))
    {
        return false;
    }
    for (int a = 0; a < 3; a++)
    {
        int32_t sum = 0;
        for (unsigned i = 0; i < stored; i++)
        {
            sum += store[i][a];
        }
        int32_t mean = sum / (int32_t)stored;
        xyz[a] = (int16_t)(sensor == ACCEL_MMA8451Q ? mean * 64 : mean);
    }
    return true;
}

accel_dir_t accel_tilt(const int16_t xyz[3])
{
    if (xyz[0] < -899 || xyz[0] > -201)
    {
        return DIR_NONE;
    }
    if (xyz[1] < -300)
    {
        return xyz[2] < -350 ? DIR_B : DIR_NONE;
    }
    if (xyz[1] > 300)
    {
        return xyz[2] > 400 ? DIR_A : DIR_NONE;
    }
    return DIR_NONE;
}

static int16_t swing(int a)
{
    int16_t lo = store[0][a], hi = store[0][a];
    for (unsigned i = 1; i < STORE; i++)
    {
        if (store[i][a] < lo)
        {
            lo = store[i][a];
        }
        if (store[i][a] > hi)
        {
            hi = store[i][a];
        }
    }
    int16_t s = (int16_t)(hi - lo);
    return (lo < 0 ? -lo : lo) > (hi < 0 ? -hi : hi) ? (int16_t)-s : s;
}

static int16_t absval(int16_t v)
{
    return v < 0 ? (int16_t)-v : v;
}

accel_dir_t accel_slap(void)
{
    bool overflow;
    uint8_t src;
    int n = sensor_read(&overflow);
    /* The source register is read even when the samples could not be:
     * reading it releases the latched interrupt line, which would
     * otherwise stay high and block every later slap. */
    bool candidate;
    if (sensor == ACCEL_LIS3DH)
    {
        if (i2c_read_regs(LIS3DH_ADDR, 0x39, &src, 1) != I2C_OK || n < 0)
        {
            return DIR_NONE;
        }
        candidate = n > 0 && stored == STORE && (src & 0x46);
    }
    else
    {
        if (i2c_read_regs(MMA8451Q_ADDR, 0x22, &src, 1) != I2C_OK || n < 0)
        {
            return DIR_NONE;
        }
        candidate = overflow && (src & 0xE0) && stored == STORE;
    }
    debug_verbose("accel: interrupt, %d samples, overflow %d, source 0x%02x, candidate %d\n", n,
                  overflow, src, candidate);
    if (!candidate)
    {
        return DIR_NONE;
    }
    int16_t x = swing(0), y = swing(1), z = swing(2);
    debug_verbose("accel: swings %d %d %d\n", x, y, z);
    int16_t m = absval(y) < absval(z) ? absval(y) : absval(z);
    if (absval(x) >= m || y == 0)
    {
        return DIR_NONE;
    }
    if (y < 0)
    {
        return z < 0 ? DIR_B : DIR_NONE;
    }
    return z > 0 ? DIR_A : DIR_NONE;
}
