/*
 * Memory layout of the application image.
 *
 * The image is loaded to 0x20004000 and runs in place. The loaded part ends
 * with the 112-byte build block, which tools/mkimage.py fills in; the slot
 * file adds the 64-byte digest text after it. Everything after the build
 * block (bss, stacks) is not part of the file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
MEMORY
{
    LOWRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x3FE0   /* free after the handover record is read */
    HANDOVER (rw): ORIGIN = 0x20003FE0, LENGTH = 0x20
    SRAM (rwx)   : ORIGIN = 0x20004000, LENGTH = 0x3C000
}

IMAGE_MAX = 0x34000;   /* slot file size, trailer included */
TRAILER   = 176;       /* build block + digest text */

ENTRY(reset_handler)

SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > SRAM

    .ARM.exidx : { *(.ARM.exidx*) } > SRAM

    .data : ALIGN(4)
    {
        *(.data*)
        . = ALIGN(4);
    } > SRAM

    /* Last loaded section: must stay the end of the image. */
    .buildblock : ALIGN(4)
    {
        KEEP(*(.buildblock))
    } > SRAM

    __image_end = .;
    ASSERT(__image_end - ORIGIN(SRAM) + 64 <= IMAGE_MAX, "image larger than an application slot")

    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > SRAM

    .lowram (NOLOAD) : ALIGN(1024)
    {
        *(.lowram.dma)
        *(.lowram*)
    } > LOWRAM

    __stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
    __stack_size = 0x700;   /* start-up before the scheduler (worst 1.4 KB), then interrupts only (nested worst about 1.4 KB) */
    ASSERT(__bss_end + __stack_size <= __stack_top, "SRAM overflow: bss and main stack do not fit")
}
