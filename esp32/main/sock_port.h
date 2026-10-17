/*
 * Sockets of the shared network code (cloud.c, rtnl.c, page.c), ESP32-S3
 * version (sock.c): lwIP sockets, TLS by mbed TLS with the box's client
 * certificate and the CA from the assets partition. No
 * server name indication and no check of the server's names.
 * Results: negative on failure, SOCK_AGAIN on a receive timeout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SOCK_PORT_H
#define SOCK_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "clock.h"

#define SOCK_AGAIN (-11)
#define SOCK_NOBUFS (-105)

bool sock_busy(long r);
bool sock_tls_full(long r);
long sock_resolve(const char *host, uint32_t *ip);
long sock_tls_connect(int16_t *s, uint32_t ip, uint16_t port, const char *ca);
long sock_send(int16_t s, const void *p, unsigned n);
long sock_recv(int16_t s, void *p, unsigned n);
void sock_close(int16_t s);
void sock_timeout(int16_t s, uint32_t ms);
long sock_dns_fallback(void);
long sock_set_date(const civil_t *cv);
long sock_http_stop(void);
int16_t sock_listen(uint16_t port);
int16_t sock_accept(int16_t l);

#endif
