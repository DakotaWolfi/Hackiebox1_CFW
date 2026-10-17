/*
 * Host build of esp32/main/assets.c: the partition and the FAT volume are
 * image files (test.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASSETS_HOST_H
#define ASSETS_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define debug_printf printf

bool part_read(uint32_t off, void *p, uint32_t n);
uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t n);
bool assets_wl_acceptable(uint32_t part_size);
bool assets_mount_host(void);
long assets_size(const char *name);
long assets_read(const char *name, uint32_t offset, void *buf, unsigned n);
long assets_replace(const char *name, const void *buf, unsigned n);
long assets_delete(const char *name);

#endif
