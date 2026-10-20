/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "inc/hw_memmap.h"
#include "inc/hw_mmchs.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#include "driverlib/sdhost.h"
#include "board.h"
#include "debug.h"
#include "journal.h"
#include "monitor.h"
#include "nfc.h"
#include "panic.h"
#include "sd.h"
#include "tick.h"

#define BASE SDHOST_BASE
#define HOST_CLOCK 120000000u

#define CMD0 (SDHOST_CMD_0)
#define CMD2 (SDHOST_CMD_2 | SDHOST_RESP_LEN_136)
#define CMD3 (SDHOST_CMD_3 | SDHOST_RESP_LEN_48)
#define CMD7 (SDHOST_CMD_7 | SDHOST_RESP_LEN_48B)
#define CMD8 (SDHOST_CMD_8 | SDHOST_RESP_LEN_48)
#define CMD9 (SDHOST_CMD_9 | SDHOST_RESP_LEN_136)
#define CMD12 (SDHOST_CMD_12 | SDHOST_RESP_LEN_48B)
#define CMD18 (SDHOST_CMD_18 | SDHOST_RD_CMD | SDHOST_RESP_LEN_48 | SDHOST_MULTI_BLK)
#define CMD25 (SDHOST_CMD_25 | SDHOST_WR_CMD | SDHOST_RESP_LEN_48 | SDHOST_MULTI_BLK)
#define CMD32 (SDHOST_CMD_32 | SDHOST_RESP_LEN_48)
#define CMD33 (SDHOST_CMD_33 | SDHOST_RESP_LEN_48)
#define CMD38 (SDHOST_CMD_38 | SDHOST_RESP_LEN_48B)
#define CMD55 (SDHOST_CMD_55 | SDHOST_RESP_LEN_48)
#define ACMD41 (SDHOST_CMD_41 | SDHOST_RESP_LEN_48)

#define CMD_LIMIT_MS 5999u
#define DATA_LIMIT_MS 5000u
#define INIT_LIMIT_MS 5000u

/* Errors of the data phase and of the command. */
#define DATA_ERRORS (SDHOST_INT_DCRC | SDHOST_INT_DEB)
#define CMD_ERRORS (SDHOST_INT_CTO | (1u << 17) /* CCRC */ | SDHOST_INT_CEB | (1u << 19) /* CIE */ | SDHOST_INT_DTO | SDHOST_INT_CERR | SDHOST_INT_BADA)

static struct
{
    bool ready;
    bool block_addressing; /* CCS = 1 */
    uint16_t rca;
    uint32_t sectors;
    uint32_t system_end; /* first data sector once mounted */
} card;

void sd_host_setup(bool pull_up, uint32_t card_clock)
{
    MAP_PinTypeSDHost(PIN_01, PIN_MODE_6);
    MAP_PinDirModeSet(PIN_01, PIN_DIR_MODE_OUT); /* clock forced to output */
    MAP_PinTypeSDHost(PIN_02, PIN_MODE_6);
    MAP_PinTypeSDHost(PIN_64, PIN_MODE_6);
    if (pull_up)
    {
        MAP_PinConfigSet(PIN_02, PIN_STRENGTH_4MA, PIN_TYPE_STD_PU);
        MAP_PinConfigSet(PIN_64, PIN_STRENGTH_4MA, PIN_TYPE_STD_PU);
    }

    MAP_PRCMPeripheralClkEnable(PRCM_SDHOST, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralReset(PRCM_SDHOST);
    MAP_SDHostInit(BASE);
    MAP_SDHostIntClear(BASE, 0xFFFFFFFFu);
    MAP_SDHostSetExpClk(BASE, HOST_CLOCK, card_clock);
}

/* supply off 100 ms, on, 200 ms, host at 12 MHz,
 * 1000 ms, identification. The NFC reader is not accessed meanwhile. */
bool sd_recover(void)
{
    card.ready = false;
    MAP_SDHostIntClear(BASE, 0xFFFFFFFFu);
    nfc_lock();
    board_sd_supply(false);
    tick_wait(100);
    board_sd_supply(true);
    tick_wait(200);
    sd_host_setup(false, 12000000u);
    nfc_unlock();
    tick_wait(1000);
    debug_printf("sd: bus recovery\n");
    return sd_identify();
}

bool sd_start(void)
{
    if (sd_identify())
    {
        return true;
    }
    uint32_t failed_at = tick_ms();
    debug_printf("sd: ident FAILED\n");
    for (;;)
    {
        if (tick_ms() - failed_at >= 20000u)
        {
            panic_from(ACT_SD, PANIC_SD_START, 0, "no card");
        }
        if (sd_recover())
        {
            return true;
        }
    }
}

uint32_t sd_host_clock(void)
{
    return MAP_PRCMPeripheralClockGet(PRCM_SDHOST);
}

void sd_host_init(void)
{
    sd_host_setup(false, 24000000u);
}

/* Sends a command and waits for completion; false on error (the host
 * reported no response, CRC or index error). A command that neither
 * completes nor fails within 5999 ms is a panic. */
static bool command(uint32_t cmd, uint32_t arg, uint32_t resp[4])
{
    MAP_SDHostIntClear(BASE, 0xFFFFFFFFu);
    if (MAP_SDHostCmdSend(BASE, cmd, arg) != 0)
    {
        return false;
    }
    uint32_t start = tick_ms();
    uint32_t st;
    do
    {
        st = MAP_SDHostIntStatus(BASE) & (SDHOST_INT_CC | SDHOST_INT_ERRI);
        if (!st && tick_ms() - start > CMD_LIMIT_MS)
        {
            panic_from(ACT_SD, PANIC_SD_HANG, cmd, "SD command hangs");
        }
    } while (!st);
    if (st & SDHOST_INT_ERRI)
    {
        MAP_SDHostCmdReset(BASE);
        return false;
    }
    if (resp)
    {
        MAP_SDHostRespGet(BASE, (unsigned long *)resp);
    }
    return true;
}

static bool wait_status(uint32_t bits, uint32_t limit_ms)
{
    uint32_t start = tick_ms();
    while (!(MAP_SDHostIntStatus(BASE) & bits))
    {
        if (tick_ms() - start > limit_ms)
        {
            return false;
        }
    }
    return true;
}

/* CRC7 over the 15 bytes before the CRC byte of an R2 response. */
static uint8_t crc7(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++)
    {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++)
        {
            crc <<= 1;
            if ((d ^ crc) & 0x80)
            {
                crc ^= 0x09;
            }
            d <<= 1;
        }
    }
    return crc & 0x7F;
}

/* The host returns the 128 register bits of an R2 response in resp[3..0]
 * (resp[3] = bits 127..96). Returns the 16 bytes, most significant first. */
static void r2_bytes(const uint32_t resp[4], uint8_t out[16])
{
    for (int i = 0; i < 16; i++)
    {
        out[i] = (uint8_t)(resp[3 - i / 4] >> (24 - 8 * (i % 4)));
    }
}

/* End bit and CRC7 of a CID or CSD. The host keeps
 * the 128 register bits with the CRC byte last; a host that strips the
 * CRC byte leaves it 0 and has then checked the CRC itself (a CRC error
 * fails the command). */
static bool r2_valid(const uint8_t b[16])
{
    if (b[15] == 0)
    {
        return true;
    }
    return (b[15] & 1) && (b[15] >> 1) == crc7(b, 15);
}

bool sd_identify(void)
{
    uint32_t r[4];

    card.ready = false;
    command(CMD0, 0, NULL);
    if (!command(CMD8, 0x000001A5u, r) || (r[0] & 0xFFF) != 0x1A5)
    {
        debug_printf("sd: CMD8 FAILED 0x%08lx\n", (unsigned long)r[0]);
        return false;
    }

    debug_verbose("sd: CMD8 answer 0x%08lx\n", (unsigned long)r[0]);
    uint32_t start = tick_ms();
    unsigned loops = 0;
    do
    {
        if (!command(CMD55, 0, NULL))
        {
            debug_printf("sd: CMD55 FAILED, %u loops\n", loops);
            return false;
        }
        if (!command(ACMD41, 0x40E00000u, r))
        {
            debug_printf("sd: ACMD41 FAILED, %u loops\n", loops);
            return false;
        }
        loops++;
        if (tick_ms() - start > INIT_LIMIT_MS)
        {
            debug_printf("sd: not ready, %u ACMD41, 0x%08lx\n", loops, (unsigned long)r[0]);
            return false;
        }
    } while (!(r[0] & 0x80000000u));
    debug_verbose("sd: ready after %u loops, OCR 0x%08lx\n", loops, (unsigned long)r[0]);
    card.block_addressing = (r[0] & 0x40000000u) != 0;

    uint8_t reg[16];
    if (!command(CMD2, 0, r))
    {
        debug_printf("sd: CMD2 FAILED\n");
        return false;
    }
    r2_bytes(r, reg);
    if (!r2_valid(reg))
    {
        debug_printf("sd: CID crc %02x bad\n", reg[15]);
        return false;
    }
    if (!command(CMD3, 0, r))
    {
        return false;
    }
    card.rca = (uint16_t)(r[0] >> 16);

    if (!command(CMD9, (uint32_t)card.rca << 16, r))
    {
        return false;
    }
    uint8_t *csd = reg;
    r2_bytes(r, csd);
    debug_verbose("sd: CSD %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                  csd[0], csd[1], csd[2], csd[3], csd[4], csd[5], csd[6], csd[7], csd[8], csd[9],
                  csd[10], csd[11], csd[12], csd[13], csd[14], csd[15]);
    unsigned structure = csd[0] >> 6;
    unsigned read_bl_len = csd[5] & 0x0F;
    unsigned write_bl_len = ((csd[12] & 0x03) << 2) | (csd[13] >> 6);
    if (!r2_valid(csd) || structure != 1 || read_bl_len != 9 || write_bl_len != 9)
    {
        debug_printf("sd: CSD crc %02x v%u bl %u/%u bad\n", csd[15], structure, read_bl_len, write_bl_len);
        return false;
    }
    uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
    card.sectors = (c_size + 1) * 1024;

    if (!command(CMD7, (uint32_t)card.rca << 16, NULL) || !wait_status(SDHOST_INT_TC, INIT_LIMIT_MS))
    {
        return false;
    }
    tick_wait(250);

    MAP_SDHostBlockSizeSet(BASE, 512);
    card.ready = true;
    return true;
}

bool sd_ready(void)
{
    return card.ready;
}

uint32_t sd_sectors(void)
{
    return card.sectors;
}

static uint32_t address(uint32_t sector)
{
    return card.block_addressing ? sector : sector * 512;
}

void sd_set_system_end(uint32_t first_data_sector)
{
    card.system_end = first_data_sector;
}

/* Resets the data line of the host after a data error or time-out. */
static void data_reset(void)
{
    HWREG(BASE + MMCHS_O_SYSCTL) |= MMCHS_SYSCTL_SRD;
    uint32_t start = tick_ms();
    while ((HWREG(BASE + MMCHS_O_SYSCTL) & MMCHS_SYSCTL_SRD) && tick_ms() - start < 10)
        ;
}

typedef enum
{
    X_OK,
    X_FAILED,
    X_TIMEOUT,
    X_DATA_ERROR
} xfer_t;

/* One CMD18 / CMD25 transfer with its CMD12. The
 * data phase and CMD12 must end within 5000 ms of the command response. */
static xfer_t transfer(bool write, uint32_t sector, uint8_t *buf, uint32_t count)
{
    monitor_sd_transfer();
    MAP_SDHostBlockCountSet(BASE, (unsigned short)count);
    if (!command(write ? CMD25 : CMD18, address(sector), NULL))
    {
        return X_FAILED;
    }
    uint32_t start = tick_ms();
    uint32_t ready = write ? MMCHS_PSTATE_BWE : MMCHS_PSTATE_BRE;
    xfer_t res = X_OK;
    for (uint32_t i = 0; i < count * 128 && res == X_OK; i++)
    {
        while (!(HWREG(BASE + MMCHS_O_PSTATE) & ready))
        {
            if (MAP_SDHostIntStatus(BASE) & SDHOST_INT_ERRI)
            {
                res = X_FAILED;
                break;
            }
            if (tick_ms() - start > DATA_LIMIT_MS)
            {
                res = X_TIMEOUT;
                break;
            }
        }
        if (res != X_OK)
        {
            break;
        }
        uint32_t w;
        if (write)
        {
            memcpy(&w, buf + 4 * i, 4);
            HWREG(BASE + MMCHS_O_DATA) = w;
        }
        else
        {
            w = HWREG(BASE + MMCHS_O_DATA);
            memcpy(buf + 4 * i, &w, 4);
        }
    }
    if (res == X_OK)
    {
        while (!(MAP_SDHostIntStatus(BASE) & (SDHOST_INT_TC | SDHOST_INT_ERRI)))
        {
            if (tick_ms() - start > DATA_LIMIT_MS)
            {
                res = X_TIMEOUT;
                break;
            }
        }
    }
    /* Checked before CMD12, which clears the status. A CRC or end-bit error
     * of the data with no command or time-out error is a data-phase error
     * of a transfer that otherwise completes. */
    uint32_t st = MAP_SDHostIntStatus(BASE);
    if (st & SDHOST_INT_ERRI)
    {
        debug_printf("sd: stat %08lx\n", (unsigned long)st); /* the error bits for the line that follows */
        if ((st & DATA_ERRORS) && !(st & CMD_ERRORS) && res != X_TIMEOUT)
        {
            res = X_DATA_ERROR;
        }
        else if (res == X_OK)
        {
            res = X_FAILED;
        }
    }
    if (res != X_OK)
    {
        data_reset();
    }
    if (!command(CMD12, 0, NULL))
    {
        if (res == X_OK)
        {
            res = X_FAILED;
        }
    }
    else
    {
        while (!(MAP_SDHostIntStatus(BASE) & (SDHOST_INT_TC | SDHOST_INT_ERRI)))
        {
            if (tick_ms() - start > DATA_LIMIT_MS)
            {
                if (res == X_OK)
                {
                    res = X_TIMEOUT;
                }
                break;
            }
        }
        if (res == X_OK && (MAP_SDHostIntStatus(BASE) & SDHOST_INT_ERRI))
        {
            res = X_FAILED;
        }
    }
    return res;
}

static const char *const xfer_text[] = {"ok", "error", "time-out", "data error"};

/* a failed transfer is followed by a bus recovery and one
 * repetition; a second failure in the system area is a panic (with a
 * sector journal after a time-out), in the data area the access fails and
 * the card stays in use. Before the volume is mounted every
 * sector counts as data area. */
static bool sd_access(bool write, uint32_t sector, uint8_t *buf, uint32_t count)
{
    if (!card.ready || count == 0)
    {
        return false;
    }
    for (int attempt = 0;; attempt++)
    {
        xfer_t r = transfer(write, sector, buf, count);
        if (r == X_OK)
        {
            return true;
        }
        debug_printf("sd: %s %lu at %lu: %s, try %d\n", write ? "write" : "read",
                     (unsigned long)count, (unsigned long)sector, xfer_text[r], attempt + 1);
        if (r == X_DATA_ERROR)
        {
            panic_from(ACT_SD, PANIC_SD_DATA, sector, "SD data error");
        }
        if (attempt == 1)
        {
            if (sector < card.system_end)
            {
                if (r == X_TIMEOUT)
                {
                    journal_write_sector(sector);
                }
                panic_from(ACT_SD, PANIC_SD_SYSTEM, sector, "SD system area failed twice");
            }
            return false;
        }
        if (!sd_recover())
        {
            panic_from(ACT_SD, PANIC_SD_RECOVERY, sector, "SD bus recovery failed");
        }
    }
}

bool sd_read(uint32_t sector, uint8_t *buf, uint32_t count)
{
    return sd_access(false, sector, buf, count);
}

bool sd_write(uint32_t sector, const uint8_t *buf, uint32_t count)
{
    return sd_access(true, sector, (uint8_t *)buf, count);
}

/* CMD32, CMD33, CMD38 and the end of busy without a time
 * limit; a failed erase is ignored. */
void sd_erase(uint32_t first, uint32_t last)
{
    if (!card.ready)
    {
        return;
    }
    monitor_sd_transfer();
    if (!command(CMD32, address(first), NULL) || !command(CMD33, address(last), NULL) ||
        !command(CMD38, 0, NULL))
    {
        debug_printf("sd: erase %lu..%lu FAILED\n", (unsigned long)first, (unsigned long)last);
        return;
    }
    uint32_t t0 = tick_ms();
    while (!(MAP_SDHostIntStatus(BASE) & (SDHOST_INT_TC | SDHOST_INT_ERRI)))
    {
        tick_wait(0);
    }
    /* A data time-out of the host ends the wait before the card: then the
     * busy level of DAT0 is followed. */
    if (MAP_SDHostIntStatus(BASE) & SDHOST_INT_ERRI)
    {
        data_reset();
        while (!(HWREG(BASE + MMCHS_O_PSTATE) & (1u << MMCHS_PSTATE_DLEV_S)))
        {
            tick_wait(0);
        }
    }
    if (tick_ms() - t0 > 1000) /* blocks the card for everyone (stall reports) */
    {
        debug_printf("sd: erase %lu..%lu %lu ms\n", (unsigned long)first, (unsigned long)last,
                     (unsigned long)(tick_ms() - t0));
    }
}
