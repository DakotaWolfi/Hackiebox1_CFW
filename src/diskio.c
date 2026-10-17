/*
 * FatFs media access on the SD card (drive 0). Failed transfers are
 * recovered and repeated in sd.c. A second drive, where the
 * platform has one, is the platform's (flashdisk.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include "ff.h"
#include "diskio.h"
#if FF_VOLUMES > 1
#include "flashdisk.h"
#endif
#include "journal.h"
#include "sd.h"

DSTATUS disk_status(BYTE drv)
{
#if FF_VOLUMES > 1
    if (drv == 1)
    {
        return flashdisk_status();
    }
#endif
    return drv == 0 && sd_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE drv)
{
#if FF_VOLUMES > 1
    if (drv == 1)
    {
        return flashdisk_status();
    }
#endif
    if (drv != 0)
    {
        return STA_NOINIT;
    }
    if (!sd_ready() && !sd_identify())
    {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE drv, BYTE *buf, LBA_t sector, UINT count)
{
#if FF_VOLUMES > 1
    if (drv == 1)
    {
        return flashdisk_read(buf, sector, count);
    }
#endif
    if (drv != 0 || !sd_ready())
    {
        return RES_NOTRDY;
    }
    return sd_read(sector, buf, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE drv, const BYTE *buf, LBA_t sector, UINT count)
{
#if FF_VOLUMES > 1
    if (drv == 1)
    {
        return flashdisk_write(buf, sector, count);
    }
#endif
    if (drv != 0 || !sd_ready())
    {
        return RES_NOTRDY;
    }
    return sd_write(sector, buf, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE drv, BYTE cmd, void *buf)
{
#if FF_VOLUMES > 1
    if (drv == 1)
    {
        return flashdisk_ioctl(cmd, buf);
    }
#endif
    if (drv != 0 || !sd_ready())
    {
        return RES_NOTRDY;
    }
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case CTRL_TRIM:
    { /* one run of released clusters */
        const LBA_t *r = buf;
        sd_erase(r[0], r[1]);
        return RES_OK;
    }
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = sd_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* Hooks of the marked patches in ff.c. The sector
 * journal is applied at the first mount only. */
uint32_t ff_tb_journal_sector(void)
{
    static bool used;
    if (used)
    {
        return JOURNAL_NONE;
    }
    used = true;
    return journal_sector();
}

void ff_tb_mounted(uint32_t first_data_sector)
{
    sd_set_system_end(first_data_sector);
}
