/*
 * Small files of the firmware in the NVS namespace "cfw":
 * one blob per file, keyed by its name (at most 15 characters). The NVS
 * partition is never erased or formatted here: when it cannot be opened,
 * the files are missing and writes fail; the namespaces of the original
 * firmware stay as they are.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "debug.h"
#include "esp32.h"

#define NS "cfw"
#define MAX_FILE 4096

static nvs_handle_t h;
static bool ready;

bool nvsfile_init(void)
{
    esp_err_t e = nvs_flash_init_partition("nvs");
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        debug_printf("nvs: partition requires erase (%d), reinitializing\n", e);
        if (nvs_flash_erase_partition("nvs") == ESP_OK)
        {
            e = nvs_flash_init_partition("nvs");
        }
    }
    if (e == ESP_OK)
    {
        e = nvs_open(NS, NVS_READWRITE, &h);
    }
    ready = e == ESP_OK;
    if (!ready)
    {
        debug_printf("nvs: not usable (%d), nothing is written\n", e);
    }
    return ready;
}

long nvsfile_size(const char *key)
{
    size_t n = 0;
    if (!ready || nvs_get_blob(h, key, NULL, &n) != ESP_OK)
    {
        return -14;
    }
    return (long)n;
}

long nvsfile_read(const char *key, uint32_t offset, void *buf, unsigned n)
{
    static uint8_t tmp[MAX_FILE];
    size_t len = sizeof tmp;
    if (!ready || nvs_get_blob(h, key, tmp, &len) != ESP_OK)
    {
        return -14;
    }
    if (offset >= len)
    {
        return 0;
    }
    if (n > len - offset)
    {
        n = (unsigned)(len - offset);
    }
    memcpy(buf, tmp + offset, n);
    return (long)n;
}

long nvsfile_write(const char *key, const void *buf, unsigned n)
{
    if (!ready || n > MAX_FILE)
    {
        return -1;
    }
    esp_err_t e = nvs_set_blob(h, key, buf, n);
    if (e == ESP_OK)
    {
        e = nvs_commit(h);
    }
    return e == ESP_OK ? (long)n : -1;
}

long nvsfile_delete(const char *key)
{
    if (!ready)
    {
        return -1;
    }
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK)
    {
        nvs_commit(h);
    }
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND ? 0 : -1;
}

/* the box language (port.h). */
bool port_language_read(uint8_t *l)
{
    return ready && nvs_get_u8(h, "lang", l) == ESP_OK;
}

bool port_language_write(uint8_t l)
{
    return ready && nvs_set_u8(h, "lang", l) == ESP_OK && nvs_commit(h) == ESP_OK;
}
