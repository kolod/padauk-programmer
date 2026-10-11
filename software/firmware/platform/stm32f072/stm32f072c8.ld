/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STM32F072C8 (64 KB flash, 16 KB RAM). Flash layout (research R5):
 *   0x08000000  44 KB  firmware
 *   0x0800B000   8 KB  package slot 0
 *   0x0800D000   8 KB  package slot 1
 *   0x0800F000   2 KB  state journal, page A
 *   0x0800F800   2 KB  state journal, page B
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 44K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

__slot0   = 0x0800B000;
__slot1   = 0x0800D000;
__journal = 0x0800F000;
__stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.ramfunc*)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* at least 2 KB of stack below __stack_top */
    ._stack (NOLOAD) : { . = ALIGN(8); . = . + 2048; } > RAM
}
