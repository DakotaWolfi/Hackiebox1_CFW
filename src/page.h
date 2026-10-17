/*
 * Setup page: a small HTTP server on port 80 of the access
 * point, run by the cloud activity while it is idle in setup mode.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PAGE_H
#define PAGE_H

#include <stdint.h>

/* Serves the setup page while net_page_on() (net.h); returns at once when
 * setup mode is not open or the card holds no /cfw/setup.htm (the vendor
 * page of the network processor's HTTP server stays then). work (4096
 * bytes) and buf (1400 bytes) are the cloud activity's buffers, which
 * nothing else uses while it runs here. */
void page_run(uint8_t *work, unsigned work_n, char *buf, unsigned buf_n);

#endif
