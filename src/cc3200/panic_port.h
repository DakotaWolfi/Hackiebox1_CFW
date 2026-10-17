/*
 * Hooks of the shared panic code (panic.c), CC3200 version: interrupt
 * masking, the reset cause and the two retained words in the hibernation
 * domain's scratch registers 2 and 3.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PANIC_PORT_H
#define PANIC_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_hib3p3.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "driverlib/utils.h"

#define START_CORE_RESET PRCM_CORE_RESET
#define START_MCU_RESET PRCM_MCU_RESET
#define START_WDT_RESET PRCM_WDT_RESET

#define HIB_DELAY ((80u * 200u) / 3u) /* 200 us settling */

static inline uint32_t port_irq_save(void)
{
    uint32_t pm;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(pm)::"memory");
    return pm;
}

static inline void port_irq_restore(uint32_t pm)
{
    __asm volatile("msr primask, %0" ::"r"(pm) : "memory");
}

static inline bool port_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) != 0;
}

static inline unsigned port_reset_cause(void)
{
    return (unsigned)MAP_PRCMSysResetCauseGet();
}

static inline void port_retained_read(uint32_t *w1, uint32_t *w2)
{
    *w1 = HWREG(HIB3P3_BASE + HIB3P3_O_MEM_HIB_REG2);
    *w2 = HWREG(HIB3P3_BASE + HIB3P3_O_MEM_HIB_REG3);
}

static inline void port_retained_write(uint32_t w1, uint32_t w2)
{
    uint32_t pm = port_irq_save();
    HWREG(HIB3P3_BASE + HIB3P3_O_MEM_HIB_REG2) = w1;
    UtilsDelay(HIB_DELAY);
    HWREG(HIB3P3_BASE + HIB3P3_O_MEM_HIB_REG3) = w2;
    UtilsDelay(HIB_DELAY);
    port_irq_restore(pm);
}

#endif
