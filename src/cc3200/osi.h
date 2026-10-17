/*
 * OS interface of the SimpleLink host driver (the names its user.h maps
 * sl_SyncObj*, sl_LockObj* and sl_Spawn to), on FreeRTOS with static
 * objects only. Implemented in sl_osi.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OSI_H
#define OSI_H

#include <stdbool.h>
#include <stddef.h>

#define OSI_WAIT_FOREVER (0xFFFFFFFF)
#define OSI_NO_WAIT (0)

/* Return values. OSI_OK is spelt as the driver's nonos.h spells it, so
 * that its definition there is an identical redefinition. */
typedef int OsiReturnVal_e;
#define NONOS_RET_OK (0)
#define OSI_OK NONOS_RET_OK
#define OSI_FAILURE (-1)
#define OSI_OPERATION_FAILED (-2)
#define OSI_INVALID_PARAMS (-4)
#define OSI_MEMORY_ALLOCATION_FAILURE (-5)

typedef unsigned int OsiTime_t;
typedef void *OsiSyncObj_t;
typedef void *OsiLockObj_t;
typedef short (*P_OSI_SPAWN_ENTRY)(void *pValue);
typedef void (*P_OSI_INTR_ENTRY)(void);
typedef void *OsiMsgQ_t;

OsiReturnVal_e osi_InterruptRegister(int iIntrNum, P_OSI_INTR_ENTRY pEntry, unsigned char ucPriority);
void osi_InterruptDeRegister(int iIntrNum);

OsiReturnVal_e osi_SyncObjCreate(OsiSyncObj_t *pSyncObj);
OsiReturnVal_e osi_SyncObjDelete(OsiSyncObj_t *pSyncObj);
OsiReturnVal_e osi_SyncObjSignal(OsiSyncObj_t *pSyncObj);
OsiReturnVal_e osi_SyncObjSignalFromISR(OsiSyncObj_t *pSyncObj);
OsiReturnVal_e osi_SyncObjWait(OsiSyncObj_t *pSyncObj, OsiTime_t Timeout);
OsiReturnVal_e osi_SyncObjClear(OsiSyncObj_t *pSyncObj);

OsiReturnVal_e osi_LockObjCreate(OsiLockObj_t *pLockObj);
OsiReturnVal_e osi_LockObjDelete(OsiLockObj_t *pLockObj);
OsiReturnVal_e osi_LockObjLock(OsiLockObj_t *pLockObj, OsiTime_t Timeout);
OsiReturnVal_e osi_LockObjUnlock(OsiLockObj_t *pLockObj);

OsiReturnVal_e osi_Spawn(P_OSI_SPAWN_ENTRY pEntry, void *pValue, unsigned long flags);

/* Message queues: used by cc_pal.c only to wait for the end of an SPI DMA
 * transfer; one binary semaphore (sl_osi.c). */
OsiReturnVal_e osi_MsgQCreate(OsiMsgQ_t *pMsgQ, char *pMsgQName, unsigned long MsgSize, unsigned long MaxMsgs);
OsiReturnVal_e osi_MsgQDelete(OsiMsgQ_t *pMsgQ);
OsiReturnVal_e osi_MsgQWrite(OsiMsgQ_t *pMsgQ, void *pMsg, OsiTime_t Timeout);
OsiReturnVal_e osi_MsgQRead(OsiMsgQ_t *pMsgQ, void *pMsg, OsiTime_t Timeout);

/* Starts the task that runs the driver's spawned functions. */
void osi_spawn_task_start(void);

/* True while some task other than the caller waits on or holds an object
 * of the driver, or a spawned function is queued or running: the
 * hand-over of a network shutdown waits for this to end. */
bool osi_driver_busy(void);

#endif
