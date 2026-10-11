/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vector table and reset handler for STM32F072. The very first action after reset forces the
 * programming supplies to 0 V (constitution, principle III), before RAM init and clocks. */
#include <stdint.h>

#include "f072.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, __stack_top;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void USB_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16 + 32])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    Default_Handler,   /* NMI */
    HardFault_Handler,
    [11] = Default_Handler,   /* SVCall */
    [14] = Default_Handler,   /* PendSV */
    [15] = SysTick_Handler,
    [16 + 31] = USB_IRQHandler,
};

void Reset_Handler(void)
{
    f072_power_early_off();   /* no RAM state used */

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
    f072_power_early_off();
    for (;;) {
    }
}
