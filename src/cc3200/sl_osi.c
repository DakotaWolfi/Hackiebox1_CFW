/*
 * OS interface of the SimpleLink host driver on FreeRTOS: pools of static
 * semaphores and mutexes, the spawn task, the network-processor interrupt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/interrupt.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "debug.h"
#include "osi.h"

/* --- sync objects (binary semaphores) and lock objects (mutexes) ---
 *
 * The driver creates 12 sync objects (the command object, one per
 * concurrent action, MAX_CONCURRENT_ACTIONS = 10, and the flow-control
 * object) and 3 locks (global, protection, flow control); 2 and 1 more
 * cover objects that are deleted while a task still waits on them.
 *
 * On a fault (_SlDriverHandleError) the driver deletes its
 * global lock while tasks may wait on it, and a network restart deletes
 * all objects. A FreeRTOS object must not be deleted or re-created while a
 * task is blocked on it, so: Delete sets *p to NULL first; waits run in
 * 20 ms slices and give up when *p no longer holds the object; a deleted
 * object that still has waiters stays allocated ("dead") until the last
 * one has left. A lock deleted by its holder is given back first, so that
 * its priority inheritance ends. */

#define N_SYNC 14
#define N_LOCK 4
#define SLICE pdMS_TO_TICKS(20)

enum
{
    FREE,
    USED,
    DEAD
};

typedef struct
{
    StaticSemaphore_t mem;
    SemaphoreHandle_t h;
    TaskHandle_t holder; /* locks only */
    uint8_t state;
    uint8_t waiters;
} obj_t;

static obj_t syncs[N_SYNC], locks[N_LOCK];

/* In a critical section. */
static obj_t *find(obj_t *o, unsigned n, void *h)
{
    if (h)
    {
        for (unsigned i = 0; i < n; i++)
        {
            if (o[i].state != FREE && o[i].h == h)
            {
                return &o[i];
            }
        }
    }
    return NULL;
}

static OsiReturnVal_e create(obj_t *o, unsigned n, void **p, bool mutex)
{
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < n; i++)
    {
        if (o[i].state == FREE)
        {
            o[i].state = USED;
            o[i].waiters = 0;
            o[i].holder = NULL;
            o[i].h = mutex ? xSemaphoreCreateMutexStatic(&o[i].mem) : xSemaphoreCreateBinaryStatic(&o[i].mem);
            *p = o[i].h;
            taskEXIT_CRITICAL();
            return OSI_OK;
        }
    }
    taskEXIT_CRITICAL();
    debug_printf("osi: no free %s object\n", mutex ? "lock" : "sync");
    return OSI_MEMORY_ALLOCATION_FAILURE;
}

/* Frees a dead object nobody waits on any more. */
static void release(obj_t *s)
{
    taskENTER_CRITICAL();
    if (s->state == DEAD && !s->waiters && !s->holder)
    {
        vSemaphoreDelete(s->h);
        s->h = NULL;
        s->state = FREE;
    }
    taskEXIT_CRITICAL();
}

static OsiReturnVal_e delete(obj_t *o, unsigned n, void **p)
{
    taskENTER_CRITICAL();
    obj_t *s = find(o, n, *p);
    *p = NULL;
    if (!s)
    {
        taskEXIT_CRITICAL();
        return OSI_INVALID_PARAMS;
    }
    s->state = DEAD;
    bool mine = s->holder && s->holder == xTaskGetCurrentTaskHandle();
    if (mine)
    {
        s->holder = NULL;
    }
    taskEXIT_CRITICAL();
    if (mine)
    {
        xSemaphoreGive(s->h);
    }
    release(s);
    return OSI_OK;
}

static OsiReturnVal_e wait(obj_t *o, unsigned n, void **p, OsiTime_t t, bool mutex)
{
    taskENTER_CRITICAL();
    void *h = *p;
    obj_t *s = find(o, n, h);
    if (!s || s->state != USED)
    {
        taskEXIT_CRITICAL();
        return OSI_OPERATION_FAILED;
    }
    s->waiters++;
    taskEXIT_CRITICAL();
    TickType_t left = t == OSI_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(t);
    OsiReturnVal_e r = OSI_OPERATION_FAILED;
    for (;;)
    {
        TickType_t w = left < SLICE ? left : SLICE;
        if (xSemaphoreTake(s->h, w) == pdTRUE)
        {
            taskENTER_CRITICAL();
            bool valid = *p == h;
            if (valid && mutex)
            {
                s->holder = xTaskGetCurrentTaskHandle();
            }
            taskEXIT_CRITICAL();
            if (valid)
            {
                r = OSI_OK;
            }
            else if (mutex)
            {
                xSemaphoreGive(s->h); /* deleted meanwhile */
            }
            break;
        }
        if (*p != h)
        {
            break;
        }
        if (left != portMAX_DELAY)
        {
            left -= w;
            if (!left)
            {
                break;
            }
        }
    }
    taskENTER_CRITICAL();
    s->waiters--;
    taskEXIT_CRITICAL();
    release(s);
    return r;
}

OsiReturnVal_e osi_SyncObjCreate(OsiSyncObj_t *p)
{
    return create(syncs, N_SYNC, p, false);
}

OsiReturnVal_e osi_SyncObjDelete(OsiSyncObj_t *p)
{
    return delete (syncs, N_SYNC, p);
}

/* Signal and clear do not block: a NULL object (deleted) is ignored. */
OsiReturnVal_e osi_SyncObjSignal(OsiSyncObj_t *p)
{
    SemaphoreHandle_t h = *p;
    if (h)
    {
        xSemaphoreGive(h);
    }
    return OSI_OK;
}

OsiReturnVal_e osi_SyncObjSignalFromISR(OsiSyncObj_t *p)
{
    SemaphoreHandle_t h = *p;
    BaseType_t woken = pdFALSE;
    if (h)
    {
        xSemaphoreGiveFromISR(h, &woken);
    }
    portYIELD_FROM_ISR(woken);
    return OSI_OK;
}

OsiReturnVal_e osi_SyncObjWait(OsiSyncObj_t *p, OsiTime_t t)
{
    return wait(syncs, N_SYNC, p, t, false);
}

OsiReturnVal_e osi_SyncObjClear(OsiSyncObj_t *p)
{
    SemaphoreHandle_t h = *p;
    if (h)
    {
        xSemaphoreTake(h, 0);
    }
    return OSI_OK;
}

OsiReturnVal_e osi_LockObjCreate(OsiLockObj_t *p)
{
    return create(locks, N_LOCK, p, true);
}

OsiReturnVal_e osi_LockObjDelete(OsiLockObj_t *p)
{
    return delete (locks, N_LOCK, p);
}

OsiReturnVal_e osi_LockObjLock(OsiLockObj_t *p, OsiTime_t t)
{
    return wait(locks, N_LOCK, p, t, true);
}

/* Gives the lock if the caller holds it; otherwise a dead lock the caller
 * still holds (deleted by another task) is given back; otherwise nothing
 * (the lock was deleted and possibly re-created meanwhile). */
OsiReturnVal_e osi_LockObjUnlock(OsiLockObj_t *p)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL();
    obj_t *s = find(locks, N_LOCK, *p);
    if (!s || s->holder != me)
    {
        s = NULL;
        for (unsigned i = 0; i < N_LOCK; i++)
        {
            if (locks[i].state == DEAD && locks[i].holder == me)
            {
                s = &locks[i];
            }
        }
    }
    if (s)
    {
        s->holder = NULL;
    }
    taskEXIT_CRITICAL();
    if (s)
    {
        xSemaphoreGive(s->h);
        release(s);
    }
    return OSI_OK;
}

/* Message queues: cc_pal.c uses one, of one int, to wait for the end of an
 * SPI DMA transfer, written by the LSPI end-of-word interrupt. A binary
 * semaphore stands for it; the message itself carries nothing. */
static StaticSemaphore_t dma_sem_buf;
static SemaphoreHandle_t dma_sem;

OsiReturnVal_e osi_MsgQCreate(OsiMsgQ_t *q, char *name, unsigned long size, unsigned long n)
{
    (void)name;
    (void)size;
    (void)n;
    if (!dma_sem)
    {
        dma_sem = xSemaphoreCreateBinaryStatic(&dma_sem_buf);
    }
    xSemaphoreTake(dma_sem, 0); /* nothing left from an earlier open */
    *q = dma_sem;
    return OSI_OK;
}

OsiReturnVal_e osi_MsgQDelete(OsiMsgQ_t *q)
{
    *q = NULL; /* the semaphore is static and kept */
    return OSI_OK;
}

OsiReturnVal_e osi_MsgQWrite(OsiMsgQ_t *q, void *m, OsiTime_t t)
{
    (void)m;
    (void)t;
    if (!*q)
    {
        return OSI_OPERATION_FAILED;
    }
    if (xPortIsInsideInterrupt())
    {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(*q, &woken);
        portYIELD_FROM_ISR(woken);
    }
    else
    {
        xSemaphoreGive(*q);
    }
    return OSI_OK;
}

OsiReturnVal_e osi_MsgQRead(OsiMsgQ_t *q, void *m, OsiTime_t t)
{
    (void)m;
    if (!*q)
    {
        return OSI_OPERATION_FAILED;
    }
    TickType_t w = t == OSI_WAIT_FOREVER ? portMAX_DELAY : (TickType_t)t;
    return xSemaphoreTake(*q, w) == pdTRUE ? OSI_OK : OSI_OPERATION_FAILED;
}

/* --- spawn: the driver hands the reading of asynchronous events to a
 * task of its own --- */

typedef struct
{
    P_OSI_SPAWN_ENTRY entry;
    void *value;
} spawn_t;

#define SPAWN_QUEUE_LEN 8
static StaticQueue_t spawn_queue_mem;
static uint8_t spawn_queue_buf[SPAWN_QUEUE_LEN * sizeof(spawn_t)];
static QueueHandle_t spawn_queue;
static volatile bool spawn_running;
static volatile uint32_t spawn_lost;

OsiReturnVal_e osi_Spawn(P_OSI_SPAWN_ENTRY entry, void *value, unsigned long flags)
{
    (void)flags;
    spawn_t s = {entry, value};
    BaseType_t woken = pdFALSE;
    if (!spawn_queue)
    {
        return OSI_OPERATION_FAILED;
    }
    BaseType_t ok = xPortIsInsideInterrupt() ? xQueueSendFromISR(spawn_queue, &s, &woken)
                                             : xQueueSend(spawn_queue, &s, 0);
    if (ok != pdTRUE)
    {
        spawn_lost++;
    }
    portYIELD_FROM_ISR(woken);
    return ok == pdTRUE ? OSI_OK : OSI_OPERATION_FAILED;
}

/* The spawned functions are the driver's reading of asynchronous messages
 * and the event handlers of net.c, which only copy and post (384 words of
 * stack). */
static void spawn_task(void *arg)
{
    (void)arg;
    uint32_t lost = 0;
    for (;;)
    {
        spawn_t s;
        if (xQueueReceive(spawn_queue, &s, portMAX_DELAY) == pdTRUE)
        {
            spawn_running = true;
            s.entry(s.value);
            spawn_running = false;
        }
        if (spawn_lost != lost)
        {
            lost = spawn_lost;
            debug_printf("osi: %lu spawns lost\n", (unsigned long)lost);
        }
    }
}

void osi_spawn_task_start(void)
{
    static StackType_t stack[384];
    static StaticTask_t tcb;
    spawn_queue = xQueueCreateStatic(SPAWN_QUEUE_LEN, sizeof(spawn_t), spawn_queue_buf, &spawn_queue_mem);
    /* Highest priority: it delivers the network processor's answers. */
    xTaskCreateStatic(spawn_task, "slspawn", 384, NULL, configMAX_PRIORITIES - 2, stack, &tcb);
}

bool osi_driver_busy(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool busy = spawn_running || (spawn_queue && uxQueueMessagesWaiting(spawn_queue));
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < N_SYNC; i++)
    {
        if (syncs[i].state != FREE && syncs[i].waiters)
        {
            busy = true;
        }
    }
    for (unsigned i = 0; i < N_LOCK; i++)
    {
        if (locks[i].state != FREE && (locks[i].waiters || (locks[i].holder && locks[i].holder != me)))
        {
            busy = true;
        }
    }
    taskEXIT_CRITICAL();
    return busy;
}

/* --- interrupts: the vector table calls nwpic_handler, which runs the
 * handler the driver registered --- */

static P_OSI_INTR_ENTRY nwpic_entry, lspi_entry;

void nwpic_handler(void)
{
    if (nwpic_entry)
    {
        nwpic_entry();
    }
}

/* End of an SPI DMA transfer to the network processor (cc_pal.c). */
void lspi_handler(void)
{
    if (lspi_entry)
    {
        lspi_entry();
    }
}

OsiReturnVal_e osi_InterruptRegister(int n, P_OSI_INTR_ENTRY entry, unsigned char prio)
{
    (void)prio;
    if (n == INT_NWPIC)
    {
        nwpic_entry = entry;
    }
    else if (n == INT_LSPI)
    {
        lspi_entry = entry;
    }
    else
    {
        return OSI_INVALID_PARAMS;
    }
    /* At the FreeRTOS syscall level, as the handlers signal sync objects
     * (the driver asks for level 1, above that level). */
    MAP_IntPrioritySet((unsigned long)n, 3 << 5);
    MAP_IntEnable((unsigned long)n);
    return OSI_OK;
}

void osi_InterruptDeRegister(int n)
{
    if (n != INT_NWPIC && n != INT_LSPI)
    {
        return;
    }
    MAP_IntDisable((unsigned long)n);
    if (n == INT_NWPIC)
    {
        nwpic_entry = NULL;
    }
    else
    {
        lspi_entry = NULL;
    }
}
