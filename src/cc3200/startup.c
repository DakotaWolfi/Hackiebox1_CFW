/*
 * Vector table and reset handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_nvic.h"
#include "driverlib/prcm.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "debug.h"
#include "handover.h"
#include "panic.h"

extern uint32_t __bss_start, __bss_end, __stack_top;
extern int main(void);

void reset_handler(void);

/* an interrupt or exception without a handler, NMI included,
 * is a panic with the active exception number. */
static void default_handler(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    panic(PANIC_EXCEPTION, ipsr & 0x1FF, "unexpected exception");
}

/* Diagnostic for faults: the stacked PC and LR and the fault status
 * registers go to the serial output (polled, no lock, only when the UART
 * has its clock), then the hard fault record and the restart. */
static void put_text(const char *s)
{
    debug_write(s, 32);
}

static void put_hex(const char *name, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char t[9];
    put_text(name);
    for (int i = 0; i < 8; i++)
    {
        t[i] = hex[(v >> (28 - 4 * i)) & 15];
    }
    t[8] = 0;
    put_text(t);
}

__attribute__((used)) static void fault_report(uint32_t *frame, uint32_t exc_return)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    put_text("\nFAULT");
    put_hex(" exception ", ipsr & 0x1FF);
    put_hex(" PC ", frame[6]);
    put_hex(" LR ", frame[5]);
    put_hex(" CFSR ", HWREG(0xE000ED28));
    put_hex(" HFSR ", HWREG(0xE000ED2C));
    put_hex(" BFAR ", HWREG(0xE000ED38));
    put_hex(" MMFAR ", HWREG(0xE000ED34));
    put_hex(" EXC_RETURN ", exc_return);
    put_text("\n");
    debug_flush();
    fault_hard(frame);
}

__attribute__((naked)) static void fault_handler(void)
{
    __asm volatile("tst lr, #4\n"
                   "ite eq\n"
                   "mrseq r0, msp\n"
                   "mrsne r0, psp\n"
                   "mov r1, lr\n"
                   "b fault_report\n");
}

void nmi_handler(void) __attribute__((weak, alias("default_handler")));
void hard_fault_handler(void) __attribute__((weak, alias("fault_handler")));
void svc_handler(void) __attribute__((weak, alias("default_handler")));
void pendsv_handler(void) __attribute__((weak, alias("default_handler")));
void systick_handler(void) __attribute__((weak, alias("default_handler")));
void i2s_handler(void) __attribute__((weak, alias("default_handler")));
void gpioa3_handler(void) __attribute__((weak, alias("default_handler")));
void nwpic_handler(void) __attribute__((weak, alias("default_handler")));
void lspi_handler(void) __attribute__((weak, alias("default_handler")));
void watchdog_handler(void) __attribute__((weak, alias("default_handler")));

/* Entries after the range initialiser override it on purpose. */
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".vectors"), used)) void (*const vectors[NUM_INTERRUPTS])(void) = {
    [0] = (void (*)(void)) & __stack_top,
    [1] = reset_handler,
    [FAULT_NMI] = nmi_handler,
    [FAULT_HARD] = hard_fault_handler,
    [FAULT_MPU] = fault_handler,
    [FAULT_BUS] = fault_handler,
    [FAULT_USAGE] = fault_handler,
    [FAULT_SVCALL] = svc_handler,
    [FAULT_DEBUG] = default_handler,
    [FAULT_PENDSV] = pendsv_handler,
    [FAULT_SYSTICK] = systick_handler,
    [16 ... NUM_INTERRUPTS - 1] = default_handler,
    [INT_I2S] = i2s_handler,
    [INT_GPIOA3] = gpioa3_handler,
    [INT_NWPIC] = nwpic_handler,
    [INT_LSPI] = lspi_handler,
    [INT_WDT] = watchdog_handler,
};

void reset_handler(void)
{
    /* Interrupts off first: whatever the bootloader left enabled must not
     * run before the handover record is read and bss is cleared. Masking
     * does not touch the low SRAM. */
    __asm volatile("cpsid i");

    /* The handover record lies below the image and must be read before
     * anything uses the low SRAM. */
    handover_capture();

    for (uint32_t *p = &__bss_start; p < &__bss_end; p++)
    {
        *p = 0;
    }

    /* Whatever the bootloader left enabled or pending is cleared, SysTick,
     * PendSV and SysTick pending bits included, then VTOR points at this
     * image's table. Interrupts stay off until the scheduler
     * starts. */
    for (int i = 0; i < 7; i++)
    {
        HWREG(NVIC_DIS0 + 4 * i) = 0xFFFFFFFFu;
        HWREG(NVIC_UNPEND0 + 4 * i) = 0xFFFFFFFFu;
    }
    HWREG(NVIC_ST_CTRL) = 0;
    HWREG(NVIC_INT_CTRL) = NVIC_INT_CTRL_UNPEND_SV | NVIC_INT_CTRL_PENDSTCLR;
    HWREG(NVIC_VTABLE) = (uint32_t)vectors;

    /* integer division by zero traps; memory
     * management, bus and usage faults stay disabled, so they escalate to
     * the hard fault. */
    HWREG(NVIC_CFG_CTRL) |= NVIC_CFG_CTRL_DIV0;
    HWREG(NVIC_SYS_HND_CTRL) &= ~(NVIC_SYS_HND_CTRL_USAGE | NVIC_SYS_HND_CTRL_BUS | NVIC_SYS_HND_CTRL_MEM);

    /* TI's prescribed MCU initialisation, before any pin is touched. */
    PRCMCC3200MCUInit();

    main();
    for (;;)
        ;
}
