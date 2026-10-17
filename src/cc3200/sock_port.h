/*
 * Sockets of the shared network code (cloud.c, rtnl.c, page.c), CC3200
 * version: the SimpleLink sockets of the network processor, which also
 * does the TLS with the certificate files of its flash. Results are those of
 * the driver: negative on failure,
 * SOCK_AGAIN on a receive timeout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SOCK_PORT_H
#define SOCK_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* newlib's sys/types.h (through stdlib.h or stdio.h) defines the select
 * macros that the driver's socket.h defines again for its own types. */
#undef FD_SETSIZE
#undef FD_SET
#undef FD_CLR
#undef FD_ISSET
#undef FD_ZERO
#undef fd_set
#include "simplelink.h"
#include "clock.h"

#define SOCK_AGAIN SL_EAGAIN
#define SOCK_NOBUFS SL_ENOBUFS

/* A connect that may succeed when tried again: a busy socket,
 * or a TLS session of the previous request not yet released. */
static inline bool sock_busy(long r)
{
    return r == SL_EAGAIN || r == SL_ESECT00MANYSSLOPENED;
}

/* All TLS sockets of the network processor are taken. */
static inline bool sock_tls_full(long r)
{
    return r == SL_ESECT00MANYSSLOPENED;
}

static inline long sock_resolve(const char *host, uint32_t *ip)
{
    unsigned long a;
    long r = sl_NetAppDnsGetHostByName((signed char *)host, (uint16_t)strlen(host), &a, SL_AF_INET);
    if (r >= 0)
    {
        *ip = (uint32_t)a;
    }
    return r;
}

/* A TLS connection with the three certificate files (key, client
 * certificate, the CA named by ca) and a receive timeout of 1 s. *s is the
 * socket (or negative) also when the connect fails, for sock_close(). */
static inline long sock_tls_connect(int16_t *s, uint32_t ip, uint16_t port, const char *ca)
{
    *s = (int16_t)sl_Socket(SL_AF_INET, SL_SOCK_STREAM, SL_SEC_SOCKET);
    if (*s < 0)
    {
        return *s;
    }
    const char *const files[3] = {"/cert/private.der", "/cert/client.der", ca};
    static const int opts[3] = {SL_SO_SECURE_FILES_PRIVATE_KEY_FILE_NAME, SL_SO_SECURE_FILES_CERTIFICATE_FILE_NAME,
                                SL_SO_SECURE_FILES_CA_FILE_NAME};
    for (int i = 0; i < 3; i++)
    {
        long r = sl_SetSockOpt(*s, SL_SOL_SOCKET, opts[i], files[i], (SlSocklen_t)strlen(files[i]));
        if (r < 0)
        {
            return r;
        }
    }
    SlTimeval_t tv = {1, 0};
    sl_SetSockOpt(*s, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof tv);
    SlSockAddrIn_t a = {0};
    a.sin_family = SL_AF_INET;
    a.sin_port = sl_Htons(port);
    a.sin_addr.s_addr = sl_Htonl(ip);
    /* SL_ESECSNOVERIFY (connected without server verification) is a
     * failure like any other. */
    return sl_Connect(*s, (SlSockAddr_t *)&a, sizeof a);
}

static inline long sock_send(int16_t s, const void *p, unsigned n)
{
    return sl_Send(s, p, (int16_t)n, 0);
}

static inline long sock_recv(int16_t s, void *p, unsigned n)
{
    return sl_Recv(s, p, (int16_t)n, 0);
}

static inline void sock_close(int16_t s)
{
    sl_Close(s);
}

static inline void sock_timeout(int16_t s, uint32_t ms)
{
    SlTimeval_t tv = {ms / 1000, ms % 1000 * 1000};
    sl_SetSockOpt(s, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof tv);
}

/* the fallback DNS server. */
static inline long sock_dns_fallback(void)
{
    SlNetCfgIpV4DnsClientArgs_t dns = {SL_IPV4_VAL(8, 8, 8, 8), 6};
    return sl_NetCfgSet(SL_IPV4_DNS_CLIENT, 0, sizeof dns, (unsigned char *)&dns);
}

/* The date the TLS checks certificates against. */
static inline long sock_set_date(const civil_t *cv)
{
    SlDateTime_t dt = {0};
    dt.sl_tm_year = cv->year;
    dt.sl_tm_mon = cv->month;
    dt.sl_tm_day = cv->day;
    dt.sl_tm_hour = cv->hour;
    dt.sl_tm_min = cv->minute;
    dt.sl_tm_sec = cv->second;
    return sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION, SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME, sizeof dt,
                     (uint8_t *)&dt);
}

/* --- the setup page server (page.c) --- */

/* Port 80 taken from the network processor's HTTP server: the result of
 * stopping it (diagnostic). */
static inline long sock_http_stop(void)
{
    return sl_NetAppStop(SL_NET_APP_HTTP_SERVER_ID);
}

/* A non-blocking listening socket on port, or a negative result. */
static inline int16_t sock_listen(uint16_t port)
{
    int16_t l = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, 0);
    SlSockAddrIn_t a = {0};
    SlSockNonblocking_t nb = {1};
    a.sin_family = SL_AF_INET;
    a.sin_port = sl_Htons(port);
    long r = l;
    if (l >= 0)
    {
        r = sl_Bind(l, (SlSockAddr_t *)&a, sizeof a);
        if (r >= 0)
        {
            r = sl_Listen(l, 2);
        }
        if (r >= 0)
        {
            r = sl_SetSockOpt(l, SL_SOL_SOCKET, SL_SO_NONBLOCKING, &nb, sizeof nb);
        }
        if (r < 0)
        {
            sl_Close(l);
        }
    }
    return (int16_t)r < 0 ? (int16_t)r : l;
}

/* An accepted connection, blocking with a receive timeout of 1 s; SOCK_AGAIN
 * when none is waiting. */
static inline int16_t sock_accept(int16_t l)
{
    SlSockAddrIn_t a;
    SlSocklen_t al = sizeof a;
    int16_t s = sl_Accept(l, (SlSockAddr_t *)&a, &al);
    if (s >= 0)
    {
        SlSockNonblocking_t nb = {0};
        SlTimeval_t tv = {1, 0};
        sl_SetSockOpt(s, SL_SOL_SOCKET, SL_SO_NONBLOCKING, &nb, sizeof nb);
        sl_SetSockOpt(s, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof tv);
    }
    return s;
}

#endif
