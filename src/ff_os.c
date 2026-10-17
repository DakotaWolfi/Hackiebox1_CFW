/*
 * FatFs re-entrancy (FF_FS_REENTRANT): one static mutex per volume and one
 * for the system, taken without a time limit (portMAX_DELAY, with
 * INCLUDE_vTaskSuspend): an access waits while another activity holds the
 * volume, for example during a free-cluster count.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FreeRTOS.h"
#include "semphr.h"
#include "ff.h"

#if INCLUDE_vTaskSuspend != 1
#error "portMAX_DELAY must wait without limit"
#endif

static StaticSemaphore_t mem[FF_VOLUMES + 1];
static SemaphoreHandle_t mutex[FF_VOLUMES + 1];

int ff_mutex_create(int vol)
{
    mutex[vol] = xSemaphoreCreateMutexStatic(&mem[vol]);
    return mutex[vol] != NULL;
}

void ff_mutex_delete(int vol)
{
    if (mutex[vol])
    {
        vSemaphoreDelete(mutex[vol]);
    }
    mutex[vol] = NULL;
}

int ff_mutex_take(int vol)
{
    return xSemaphoreTake(mutex[vol], portMAX_DELAY) == pdTRUE;
}

void ff_mutex_give(int vol)
{
    xSemaphoreGive(mutex[vol]);
}
