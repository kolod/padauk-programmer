/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Clocks: SYSCLK 170 MHz (range 1 boost) from PLL: HSE 8 MHz / 2 × 85 / 2, or HSI16 / 4 × 85 / 2
 * if the crystal does not start. APB1/APB2 at 170 MHz. USB and RNG from HSI48 trimmed by CRS. */
#include "g474.h"

uint32_t SystemCoreClock = 16000000u;

void SystemInit(void) {}

void g474_clock_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR5 &= ~PWR_CR5_R1MODE;   /* range 1 boost: up to 170 MHz */
    /* read-modify-write: ACR also holds DBG_SWEN, and clearing it disables SWD */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN |
                 FLASH_ACR_DCEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {
    }

    RCC->CR |= RCC_CR_HSEON;
    uint32_t n = 400000;
    while (!(RCC->CR & RCC_CR_HSERDY) && --n) {
    }
    bool hse = n != 0;
    if (!hse)
        RCC->CR &= ~RCC_CR_HSEON;

    uint32_t m = hse ? 2u : 4u;   /* PLL input 4 MHz */
    RCC->PLLCFGR = (hse ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI) | ((m - 1u) << RCC_PLLCFGR_PLLM_Pos) |
                   (85u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLR_Pos) /* R = 2 */ | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }

    /* above 80 MHz: switch with AHB / 2 for at least 1 µs, then full speed (RM0440 §7.2.7) */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_SW)) | RCC_CFGR_HPRE_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    for (volatile int i = 0; i < 200; i++) {
    }
    RCC->CFGR &= ~RCC_CFGR_HPRE;
    SystemCoreClock = G474_HCLK_HZ;

    /* HSI48 for USB and RNG (CCIPR.CLK48SEL = 00 after reset), trimmed on USB SOF */
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {
    }
    RCC->APB1ENR1 |= RCC_APB1ENR1_CRSEN;
    CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | CRS_CFGR_SYNCSRC_1;
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    PWR->CR3 |= PWR_CR3_UCPD_DBDIS;   /* release the UCPD dead-battery pull-downs on PB4/PB6 */
}
