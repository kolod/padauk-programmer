/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vector table and reset handler for STM32G474. Supplies go to 0 V first, then the FPU is enabled.
 * NMI also handles flash ECC double errors (a word torn by a power loss): the flag is cleared and the
 * read returns garbage, which the package CRC catches. */
#include <stdint.h>

#include "g474.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, __stack_top;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void NMI_Handler(void);
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void USB_HP_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_LP_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USBWakeUp_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

volatile uint32_t g474_ecc_errors;

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16 + 102])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
    [11] = Default_Handler,   /* SVCall */
    [12] = Default_Handler,   /* DebugMon */
    [14] = Default_Handler,   /* PendSV */
    [15] = SysTick_Handler,
    [16 + 19] = USB_HP_IRQHandler,
    [16 + 20] = USB_LP_IRQHandler,
    [16 + 42] = USBWakeUp_IRQHandler,
};

void Reset_Handler(void)
{
    g474_supplies_early_off();
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

void NMI_Handler(void)
{
    if (FLASH->ECCR & FLASH_ECCR_ECCD) {
        FLASH->ECCR |= FLASH_ECCR_ECCD;   /* write 1 to clear */
        g474_ecc_errors++;
        return;
    }
    Default_Handler();
}

void Default_Handler(void)
{
    g474_supplies_early_off();
    for (;;) {
    }
}
