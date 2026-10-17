/*
 * Files of the box's own flash (not the card), by their names on the
 * CC3200 serial flash ("/cert/ca.der", "/settings.cfg", "/journal.bin",
 * "/fw/sfx.bin", ...). Each platform maps them (src/cc3200/flashfs.c:
 * the network processor's file system; esp32/main/flashfs.c: the assets
 * partition and the NVS namespace of the firmware).
 *
 * Results: >= 0 success, a negative platform error code otherwise;
 * FLASHFS_MISSING (port.h) when the file does not exist.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FLASHFS_H
#define FLASHFS_H

#include <stdbool.h>
#include <stdint.h>

/* The file's length. */
long flashfs_size(const char *name);
/* Up to n bytes from offset: the count read. */
long flashfs_read(const char *name, uint32_t offset, void *buf, unsigned n);
/* The whole file replaced by n bytes, in place where it exists with room
 * for them, else created fail-safe with room for at least 64 bytes: n on
 * success. */
long flashfs_write(const char *name, const void *buf, unsigned n);
/* The file deleted and created again with exactly n bytes, fail-safe or
 * not: n on success. */
long flashfs_replace(const char *name, const void *buf, unsigned n, bool failsafe);
long flashfs_delete(const char *name);

#endif
