/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STM32G474CE (512 KB flash in two 256 KB banks, 96 KB SRAM1+SRAM2; CCM SRAM unused).
 * Bank 1 (0x08000000, 256 KB): firmware. Bank 2 (0x08040000): package slots and the state journal,
 * see hal_g474_flash.c. */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

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

    ._stack (NOLOAD) : { . = ALIGN(8); . = . + 8192; } > RAM
}
