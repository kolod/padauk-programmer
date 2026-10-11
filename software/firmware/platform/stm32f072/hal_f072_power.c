/* SPDX-License-Identifier: GPL-3.0-or-later */
/* VDD/VPP of the Easy PDK board: DAC → op-amp regulators, ADC on /6 dividers, VREFINT for VDDA.
 * Scaling constants as in easy-pdk-programmer-software/Firmware/source/Src/fpdk.c
 * (Copyright (C) 2019-2020 freepdk https://free-pdk.github.io, GPL-3.0-or-later). */
#include "f072.h"
#include "hal_power.h"

#define VDD_DAC_MAX_MV (6290u - 30u)    /* DAC full scale → mV after the op-amp (−30 mV offset) */
#define VPP_DAC_MAX_MV (13300u - 30u)
#define VREFINT_CAL (*(const uint16_t *)0x1FFFF7BAu)   /* VREFINT at VDDA = 3.3 V, factory */
#define ADC_CH_VREFINT 17u

static uint32_t ch_vpp = 8, ch_vdd = 9;   /* PB0/PB1; mini-pill: PA0/PA1 */
static uint16_t dac_vdd, dac_vpp;

void f072_power_early_off(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;
    GPIOA->MODER |= (3u << 8) | (3u << 10);   /* PA4, PA5 analog */
    DAC->DHR12RD = 0;
    DAC->CR = DAC_CR_EN1 | DAC_CR_EN2;
    GPIOB->MODER &= ~(0x3FFu << 6);           /* PB3..PB7 (IC lines) inputs = Hi-Z */
    GPIOB->PUPDR &= ~(0x3FFu << 6);
}

static void dac_update(void) { DAC->DHR12RD = ((uint32_t)dac_vdd << 16) | dac_vpp; }

uint16_t f072_adc_convert(uint32_t channel)
{
    ADC1->CHSELR = 1u << channel;
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC)) {
    }
    return (uint16_t)ADC1->DR;
}

void f072_power_init(void)
{
    /* hardware variant: PB8 = GND → Lite, PB9 = GND → mini pill (ADC on PA0/PA1) */
    GPIOB->MODER &= ~(0xFu << 16);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(0xFu << 16)) | (0x5u << 16);
    for (volatile int i = 0; i < 1000; i++) {
    }
    if (!(GPIOB->IDR & (1u << 9)) && (GPIOB->IDR & (1u << 8))) {
        ch_vpp = 0;
        ch_vdd = 1;
        GPIOA->MODER |= 0xFu;   /* PA0, PA1 analog */
    } else {
        GPIOB->MODER |= 0xFu;   /* PB0, PB1 analog */
    }

    /* ADC: HSI14 asynchronous clock, calibration, 71.5-cycle sampling, VREFINT + temperature on */
    RCC->APB2ENR |= RCC_APB2ENR_ADCEN;
    ADC1->CFGR2 = 0;
    if (ADC1->CR & ADC_CR_ADEN) {
        ADC1->CR |= ADC_CR_ADDIS;
        while (ADC1->CR & ADC_CR_ADEN) {
        }
    }
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {
    }
    ADC1->SMPR = 6u;
    ADC1_COMMON->CCR |= ADC_CCR_VREFEN | ADC_CCR_TSEN;
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {
    }

    /* −4.8 V charge pump: TIM15_CH2 on PA3 (AF0), 48 MHz / 96 / 11 ≈ 45 kHz, 50 % */
    RCC->APB2ENR |= RCC_APB2ENR_TIM15EN;
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 6)) | (2u << 6);
    GPIOA->AFR[0] &= ~(0xFu << 12);
    TIM15->PSC = 95;
    TIM15->ARR = 10;
    TIM15->CCR2 = 5;
    TIM15->CCMR1 = (6u << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;
    TIM15->CCER = TIM_CCER_CC2E;
    TIM15->BDTR = TIM_BDTR_MOE;
    TIM15->EGR = TIM_EGR_UG;
    TIM15->CR1 = TIM_CR1_CEN;

    /* 15 V booster for VPP: PA2 high */
    GPIOA->BSRR = 1u << 2;
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 4)) | (1u << 4);

    dac_vdd = dac_vpp = 0;
    dac_update();
}

void hal_power_set(pdk_rail_t rail, uint16_t mV)
{
    uint32_t max = rail == PDK_RAIL_VDD ? VDD_DAC_MAX_MV : VPP_DAC_MAX_MV;
    uint32_t v = (uint32_t)mV * 4095u / max;
    if (v > 4095u)
        v = 4095u;
    if (rail == PDK_RAIL_VDD)
        dac_vdd = (uint16_t)v;
    else
        dac_vpp = (uint16_t)v;
    dac_update();
}

uint16_t hal_power_measure(pdk_rail_t rail)
{
    uint32_t ch = rail == PDK_RAIL_VDD ? ch_vdd : ch_vpp;
    uint32_t raw = 0, ref = 0;
    for (int i = 0; i < 2; i++) {
        raw += f072_adc_convert(ch);
        ref += f072_adc_convert(ADC_CH_VREFINT);
    }
    if (!ref)
        return 0;
    /* V = raw/4095 · VDDA · 6,  VDDA = 3300 · VREFINT_CAL / ref */
    uint64_t mv = (uint64_t)raw * 3300u * VREFINT_CAL * 6u / ((uint64_t)ref * 4095u);
    return mv > 0xFFFFu ? 0xFFFFu : (uint16_t)mv;
}

void hal_power_off_all(void)
{
    dac_vdd = dac_vpp = 0;
    DAC->DHR12RD = 0;
}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = VDD_DAC_MAX_MV;
    *vpp_max_mV = VPP_DAC_MAX_MV;
}
