/*
 * Server block in the image: 80 bytes that a server fills in
 * before it delivers the image through the original firmware's update. At
 * every start, when /cfw/config.txt has no valid host
 * line and the block holds a host that passes the host checks of
 * /cfw/config.txt, its host and port become the host and port lines of the
 * file; the other lines stay. A valid host line on the card always wins.
 *
 * The marker occurs once in the image: the code reads it from the block
 * and never names it as a literal of its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cloud.h"
#include "debug.h"
#include "esp32.h"
#include "ff.h"

typedef struct
{
    char marker[8];
    char host[64];
    uint8_t port[2];
    uint8_t reserved[6];
} srvblock_t;

_Static_assert(sizeof(srvblock_t) == 80, "server block of 80 bytes");

__attribute__((used)) static const volatile srvblock_t block = {
    .marker = {'C', 'F', 'W', 'S', 'R', 'V', '0', '1'},
};

#define CONFIG "/cfw/config.txt"

static char text[1100], check[1100];

/* True when a line "host=..." is there (whatever its value). */
static bool has_host_line(const char *t)
{
    for (const char *l = t; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
    {
        while (*l == ' ' || *l == '\t')
        {
            l++;
        }
        if (!strncasecmp(l, "host", 4))
        {
            const char *e = l + 4;
            while (*e == ' ' || *e == '\t')
            {
                e++;
            }
            if (*e == '=')
            {
                return true;
            }
        }
    }
    return false;
}

/* Removes the host and port lines. */
static void drop_server_lines(char *t)
{
    char *out = t;
    for (char *l = t; *l;)
    {
        char *nl = strchr(l, '\n');
        size_t n = nl ? (size_t)(nl - l + 1) : strlen(l);
        const char *k = l;
        while (*k == ' ' || *k == '\t')
        {
            k++;
        }
        bool drop = !strncasecmp(k, "host", 4) || !strncasecmp(k, "port", 4);
        if (drop)
        {
            const char *e = k + 4;
            while (*e == ' ' || *e == '\t')
            {
                e++;
            }
            drop = *e == '=';
        }
        if (!drop)
        {
            memmove(out, l, n);
            out += n;
        }
        l += n;
    }
    *out = 0;
}

void srvblock_apply(void)
{
    char host[65];
    for (int i = 0; i < 64; i++)
    {
        host[i] = block.host[i];
    }
    host[64] = 0;
    uint16_t port = (uint16_t)(block.port[0] | block.port[1] << 8);
    if (!host[0])
    {
        return; /* not filled in */
    }
    FIL f;
    UINT n = 0;
    text[0] = 0;
    if (f_open(&f, CONFIG, FA_READ) == FR_OK)
    {
        if (f_read(&f, text, 1024, &n) != FR_OK)
        {
            n = 0;
        }
        f_close(&f);
        text[n] = 0;
    }
    strcpy(check, text);
    if (has_host_line(text) && !cloud_config_check(check))
    {
        return; /* a valid host line on the card wins */
    }
    drop_server_lines(text);
    size_t l = strlen(text);
    if (l && text[l - 1] != '\n')
    {
        text[l++] = '\n';
    }
    int k = snprintf(text + l, sizeof text - l, "host=%s\nport=%u\n", host, port ? port : 443u);
    if (k <= 0 || l + (size_t)k > 1024)
    {
        debug_printf("srvblock: config would be too long\n");
        return;
    }
    strcpy(check, text);
    const char *why = cloud_config_check(check);
    if (why)
    {
        debug_printf("srvblock: host refused (%s)\n", why);
        return;
    }
    f_mkdir("/cfw");
    UINT w = 0;
    n = (UINT)strlen(text);
    if (f_open(&f, "/cfw/config.tmp", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        return;
    }
    FRESULT e = f_write(&f, text, n, &w);
    if ((f_close(&f) | e) != FR_OK || w != n)
    {
        f_unlink("/cfw/config.tmp");
        return;
    }
    f_unlink(CONFIG);
    bool ok = f_rename("/cfw/config.tmp", CONFIG) == FR_OK;
    debug_printf("srvblock: %s port %u %s\n", host, port ? port : 443u, ok ? "written to " CONFIG : "NOT written");
}
