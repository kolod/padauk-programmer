/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Clocks: SYSCLK 48 MHz from HSE 8 MHz × 6 (HSI/2 × 12 if the crystal does not start), USB from
 * HSI48 trimmed by CRS on USB SOF, ADC from HSI14. */
#include "f072.h"

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, uint32_t loops)
{
    while (loops--)
        if (*reg & mask)
            return true;
    return false;
}

void f072_clock_init(void)
{
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY;   /* 1 wait state for 48 MHz */

    RCC->CR |= RCC_CR_HSEON;
    bool hse = wait_flag(&RCC->CR, RCC_CR_HSERDY, 200000);
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) {
    }
    if (hse) {
        RCC->CFGR2 = RCC_CFGR2_PREDIV_DIV1;
        RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLMUL)) | RCC_CFGR_PLLSRC_HSE_PREDIV | RCC_CFGR_PLLMUL6;
    } else {
        RCC->CR &= ~RCC_CR_HSEON;
        RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLMUL)) | RCC_CFGR_PLLSRC_HSI_DIV2 | RCC_CFGR_PLLMUL12;
    }
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE)) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }

    /* HSI48 for USB, trimmed by the clock recovery system on USB SOF */
    RCC->CR2 |= RCC_CR2_HSI48ON;
    while (!(RCC->CR2 & RCC_CR2_HSI48RDY)) {
    }
    RCC->CFGR3 &= ~RCC_CFGR3_USBSW;   /* USB clock = HSI48 */
    RCC->APB1ENR |= RCC_APB1ENR_CRSEN;
    CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | CRS_CFGR_SYNCSRC_1;   /* USB SOF */
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    /* HSI14 for the ADC */
    RCC->CR2 |= RCC_CR2_HSI14ON;
    while (!(RCC->CR2 & RCC_CR2_HSI14RDY)) {
    }

    SystemCoreClock = F072_HCLK_HZ;
}

uint32_t SystemCoreClock = 8000000u;

void SystemInit(void) {}
