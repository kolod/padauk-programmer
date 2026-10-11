/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Clocks: SYSCLK 96 MHz from PLL (HSE 25 MHz, HSI 16 MHz if the crystal does not start),
 * USB 48 MHz from the same PLL, APB1 48 MHz, APB2 96 MHz. */
#include "f411.h"

uint32_t SystemCoreClock = 16000000u;

void SystemInit(void) {}

void f411_clock_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR = (PWR->CR & ~PWR_CR_VOS) | PWR_CR_VOS;   /* scale 1: up to 100 MHz */

    RCC->CR |= RCC_CR_HSEON;
    uint32_t n = 400000;
    while (!(RCC->CR & RCC_CR_HSERDY) && --n) {
    }
    bool hse = n != 0;
    if (!hse)
        RCC->CR &= ~RCC_CR_HSEON;

    uint32_t m = hse ? 25u : 16u;   /* VCO input 1 MHz */
    RCC->PLLCFGR = (m << RCC_PLLCFGR_PLLM_Pos) | (192u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLP_Pos) |
                   (4u << RCC_PLLCFGR_PLLQ_Pos) | (hse ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }

    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_3WS;
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW)) |
                RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    SystemCoreClock = F411_HCLK_HZ;
}
