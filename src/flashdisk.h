/*
 * The second FatFs drive ("1:"), when a platform has one (FF_VOLUMES 2):
 * a FAT volume in the platform's flash, on the ESP32 box the assets
 * partition. diskio.c hands every access to drive 1 to these
 * functions, with FatFs's arguments and results.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FLASHDISK_H
#define FLASHDISK_H

#include "ff.h"
#include "diskio.h"

DSTATUS flashdisk_status(void);
DRESULT flashdisk_read(BYTE *buf, LBA_t sector, UINT count);
DRESULT flashdisk_write(const BYTE *buf, LBA_t sector, UINT count);
DRESULT flashdisk_ioctl(BYTE cmd, void *buf);

#endif
