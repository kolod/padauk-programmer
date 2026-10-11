/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vector table and reset handler for STM32F411. Enables the FPU before any C code runs. */
#include <stdint.h>

#include "f411.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, __stack_top;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void OTG_FS_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16 + 86])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    Default_Handler,   /* NMI */
    HardFault_Handler,
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
    [11] = Default_Handler,   /* SVCall */
    [12] = Default_Handler,   /* DebugMon */
    [14] = Default_Handler,   /* PendSV */
    [15] = SysTick_Handler,
    [16 + 67] = OTG_FS_IRQHandler,
};

void Reset_Handler(void)
{
    SCB->CPACR |= (0xFu << 20);   /* CP10, CP11 full access */
    __DSB();
    __ISB();

    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;)
        *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss;)
        *dst++ = 0;
    main();
    for (;;) {
    }
}

void Default_Handler(void)
{
    for (;;) {
    }
}
