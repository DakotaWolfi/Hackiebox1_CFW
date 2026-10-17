/*
 * Sockets and TLS (sock_port.h) on lwIP and mbed TLS.
 *
 * A TLS connection: TLS 1.2, the box's
 * client certificate and key, the server certificate checked against the
 * CA named by the server configuration, no server name indication and no
 * check of the server's names. The three files come from the flash files
 * (flashfs.h: /cert/..., the copies in /CFW/ of the assets partition);
 * they are read again for every connection, so a changed CA
 * takes effect at once.
 *
 * Handles: TLS connections are SOCK_TLS_BASE + slot, plain sockets (the
 * setup page) are lwIP descriptors, which are smaller.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "clock.h"
#include "debug.h"
#include "flashfs.h"
#include "port.h"
#include "sock_port.h"

#define SOCK_TLS_BASE 200
#define TLS_SLOTS 3
#define CERT_MAX 4096

/* Results beyond the lwIP errors: the TLS layer's, offset so that they
 * stay negative and distinct. */
#define SOCK_ERR_FILE (-454) /* a certificate file missing or unreadable */
#define SOCK_ERR_TLS (-453)  /* handshake failed, server not verified */
#define SOCK_ERR_BUSY (-460) /* all TLS slots in use */

typedef struct
{
    bool used;
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca, own;
    mbedtls_pk_context key;
} tls_t;

static tls_t tls[TLS_SLOTS];
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static bool rng_ready;
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t lock;
static uint8_t file_buf[CERT_MAX];

static void init_once(void)
{
    if (lock)
    {
        return;
    }
    CRITICAL_ENTER();
    if (!lock)
    {
        lock = xSemaphoreCreateMutexStatic(&lock_mem);
    }
    CRITICAL_EXIT();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!rng_ready)
    {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        rng_ready = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"tb", 2) == 0;
    }
    xSemaphoreGive(lock);
}

bool sock_busy(long r)
{
    return r == SOCK_AGAIN || r == SOCK_ERR_BUSY;
}

bool sock_tls_full(long r)
{
    return r == SOCK_ERR_BUSY;
}

long sock_resolve(const char *host, uint32_t *ip)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int r = lwip_getaddrinfo(host, NULL, &hints, &res);
    if (r != 0 || !res)
    {
        return r ? -r - 1000 : -1000;
    }
    *ip = lwip_ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    lwip_freeaddrinfo(res);
    return 0;
}

static tls_t *slot_of(int16_t s)
{
    int i = s - SOCK_TLS_BASE;
    return i >= 0 && i < TLS_SLOTS && tls[i].used ? &tls[i] : NULL;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = lwip_send(fd, buf, len, 0);
    if (n < 0)
    {
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = lwip_recv(fd, buf, len, 0);
    if (n < 0)
    {
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (n == 0)
    {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    return n;
}

static void set_timeout(int fd, uint32_t ms)
{
    struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = ms % 1000 * 1000};
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* A whole flash file into file_buf (under the lock). */
static long load(const char *name)
{
    long n = flashfs_size(name);
    if (n <= 0 || n > CERT_MAX)
    {
        return n <= 0 ? SOCK_ERR_FILE : SOCK_ERR_FILE;
    }
    return flashfs_read(name, 0, file_buf, (unsigned)n) == n ? n : SOCK_ERR_FILE;
}

static void tls_free(tls_t *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_x509_crt_free(&t->own);
    mbedtls_pk_free(&t->key);
    if (t->fd >= 0)
    {
        lwip_close(t->fd);
    }
    t->fd = -1;
    t->used = false;
}

/* The certificates and the configuration; 0 or a negative result. */
static long tls_setup(tls_t *t, const char *ca)
{
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_x509_crt_init(&t->own);
    mbedtls_pk_init(&t->key);
    long n = load(ca);
    if (n < 0 || mbedtls_x509_crt_parse_der(&t->ca, file_buf, (size_t)n))
    {
        debug_printf("sock: CA %s not usable\n", ca);
        return SOCK_ERR_FILE;
    }
    n = load("/cert/client.der");
    if (n < 0 || mbedtls_x509_crt_parse_der(&t->own, file_buf, (size_t)n))
    {
        debug_printf("sock: client certificate not usable\n");
        return SOCK_ERR_FILE;
    }
    n = load("/cert/private.der");
    if (n < 0 || mbedtls_pk_parse_key(&t->key, file_buf, (size_t)n, NULL, 0, mbedtls_ctr_drbg_random, &drbg))
    {
        debug_printf("sock: private key not usable\n");
        return SOCK_ERR_FILE;
    }
    memset(file_buf, 0, sizeof file_buf);
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
    {
        return SOCK_ERR_TLS;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (mbedtls_ssl_conf_own_cert(&t->conf, &t->own, &t->key) || mbedtls_ssl_setup(&t->ssl, &t->conf))
    {
        return SOCK_ERR_TLS;
    }
    /* no server name indication, no check of the names. */
    mbedtls_ssl_set_hostname(&t->ssl, NULL);
    return 0;
}

long sock_tls_connect(int16_t *s, uint32_t ip, uint16_t port, const char *ca)
{
    init_once();
    *s = -1;
    if (!rng_ready)
    {
        return SOCK_ERR_TLS;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    tls_t *t = NULL;
    int i;
    for (i = 0; i < TLS_SLOTS; i++)
    {
        if (!tls[i].used)
        {
            t = &tls[i];
            break;
        }
    }
    if (!t)
    {
        xSemaphoreGive(lock);
        return SOCK_ERR_BUSY;
    }
    t->used = true;
    t->fd = -1;
    long r = tls_setup(t, ca);
    xSemaphoreGive(lock);
    *s = (int16_t)(SOCK_TLS_BASE + i);
    if (r < 0)
    {
        return r;
    }
    t->fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->fd < 0)
    {
        return errno == ENOBUFS || errno == ENOMEM ? SOCK_NOBUFS : -errno;
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = lwip_htons(port)};
    a.sin_addr.s_addr = lwip_htonl(ip);
    if (lwip_connect(t->fd, (struct sockaddr *)&a, sizeof a) < 0)
    {
        return errno == ENOBUFS || errno == ENOMEM ? SOCK_NOBUFS : -errno;
    }
    set_timeout(t->fd, 10000);
    mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);
    int h;
    while ((h = mbedtls_ssl_handshake(&t->ssl)) != 0)
    {
        if (h != MBEDTLS_ERR_SSL_WANT_READ && h != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            debug_printf("sock: handshake -0x%04x, verify 0x%lx\n", -h, (unsigned long)flags);
            return SOCK_ERR_TLS;
        }
    }
    set_timeout(t->fd, 1000);
    return 0;
}

long sock_send(int16_t s, const void *p, unsigned n)
{
    tls_t *t = slot_of(s);
    if (!t)
    {
        int r = lwip_send(s, p, n, 0);
        return r < 0 ? (errno == EAGAIN ? SOCK_AGAIN : -errno) : r;
    }
    int r = mbedtls_ssl_write(&t->ssl, p, n);
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ)
    {
        return SOCK_AGAIN;
    }
    return r < 0 ? SOCK_ERR_TLS : r;
}

long sock_recv(int16_t s, void *p, unsigned n)
{
    tls_t *t = slot_of(s);
    if (!t)
    {
        int r = lwip_recv(s, p, n, 0);
        return r < 0 ? (errno == EAGAIN || errno == EWOULDBLOCK ? SOCK_AGAIN : -errno) : r;
    }
    int r = mbedtls_ssl_read(&t->ssl, p, n);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        return SOCK_AGAIN;
    }
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
        return 0;
    }
    return r < 0 ? SOCK_ERR_TLS : r;
}

void sock_close(int16_t s)
{
    tls_t *t = slot_of(s);
    if (!t)
    {
        if (s >= 0)
        {
            lwip_close(s);
        }
        return;
    }
    if (t->fd >= 0)
    {
        mbedtls_ssl_close_notify(&t->ssl);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    tls_free(t);
    xSemaphoreGive(lock);
}

void sock_timeout(int16_t s, uint32_t ms)
{
    tls_t *t = slot_of(s);
    int fd = t ? t->fd : s;
    if (fd >= 0)
    {
        set_timeout(fd, ms);
    }
}

/* the fallback DNS server after the one of the network. */
long sock_dns_fallback(void)
{
    ip_addr_t a;
    IP_ADDR4(&a, 8, 8, 8, 8);
    dns_setserver(1, &a);
    return 0;
}

/* The date the certificate checks use: the system time. */
long sock_set_date(const civil_t *cv)
{
    struct timeval tv = {.tv_sec = (time_t)clock_from_civil(cv)};
    return settimeofday(&tv, NULL);
}

long sock_http_stop(void)
{
    return 0;
}

int16_t sock_listen(uint16_t port)
{
    int l = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l < 0)
    {
        return (int16_t)-errno;
    }
    int one = 1;
    lwip_setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = lwip_htons(port)};
    a.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
    if (lwip_bind(l, (struct sockaddr *)&a, sizeof a) < 0 || lwip_listen(l, 2) < 0)
    {
        int e = errno;
        lwip_close(l);
        return (int16_t)-e;
    }
    lwip_fcntl(l, F_SETFL, lwip_fcntl(l, F_GETFL, 0) | O_NONBLOCK);
    return (int16_t)l;
}

int16_t sock_accept(int16_t l)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int s = lwip_accept(l, (struct sockaddr *)&a, &al);
    if (s < 0)
    {
        return errno == EAGAIN || errno == EWOULDBLOCK ? SOCK_AGAIN : (int16_t)-errno;
    }
    lwip_fcntl(s, F_SETFL, lwip_fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    set_timeout(s, 1000);
    return (int16_t)s;
}
