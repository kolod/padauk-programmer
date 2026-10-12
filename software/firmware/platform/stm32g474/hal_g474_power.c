/* SPDX-License-Identifier: GPL-3.0-or-later */
/* VDD/VPP through the on-chip DAC1 and ADC1 (provisional front end, see g474.h). The scaling follows
 * the Easy PDK board: DAC full scale → 6.26 V VDD / 13.27 V VPP after the regulators, ADC inputs on
 * /6 dividers, VDDA from VREFINT (calibrated at 3.0 V on the G4).
 * Without PDK_G474_SUPPLIES nothing is driven: limits are 0 V, measurements 0 V, and every nonzero
 * set point ends in VDD_FAULT/VPP_FAULT. */
#include "g474.h"
#include "hal_power.h"

#ifdef PDK_G474_SUPPLIES

#define VDD_DAC_MAX_MV (6290u - 30u)
#define VPP_DAC_MAX_MV (13300u - 30u)
#define DIVIDER 6u
#define VREFINT_CAL (*(const uint16_t *)0x1FFF75AAu)   /* VREFINT at VDDA = 3.0 V, factory */
#define VREFINT_CAL_MV 3000u
#define CH_VDD 1u    /* PA0 */
#define CH_VPP 2u    /* PA1 */
#define CH_VREFINT 18u

static uint16_t dac_vdd, dac_vpp;

void g474_supplies_early_off(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_DAC1EN;
    (void)RCC->AHB2ENR;
    GPIOA->MODER |= (3u << 8) | (3u << 10);   /* PA4, PA5 analog (reset state) */
    DAC1->MCR = DAC_MCR_HFSEL_1;               /* AHB > 160 MHz once the PLL runs */
    DAC1->DHR12RD = 0;
    DAC1->CR = DAC_CR_EN1 | DAC_CR_EN2;
}

static void dac_update(void) { DAC1->DHR12RD = ((uint32_t)dac_vdd << 16) | dac_vpp; }

static uint16_t adc_convert(uint32_t ch)
{
    ADC1->SQR1 = ch << ADC_SQR1_SQ1_Pos;   /* L = 0: one conversion */
    ADC1->ISR = ADC_ISR_EOC;
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC)) {
    }
    return (uint16_t)ADC1->DR;
}

void g474_power_init(void)
{
    GPIOA->MODER |= 0xFu;   /* PA0, PA1 analog */
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
    (void)RCC->AHB2ENR;
    ADC12_COMMON->CCR = (3u << ADC_CCR_CKMODE_Pos) | ADC_CCR_VREFEN;   /* HCLK / 4 = 42.5 MHz */
    ADC1->CR = 0;                      /* leave deep power-down */
    ADC1->CR = ADC_CR_ADVREGEN;
    for (volatile int i = 0; i < 20000; i++) {   /* tADCVREG_STUP = 20 µs */
    }
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {
    }
    ADC1->SMPR1 = (6u << ADC_SMPR1_SMP1_Pos) | (6u << ADC_SMPR1_SMP2_Pos);   /* 247.5 cycles */
    ADC1->SMPR2 = 7u << ADC_SMPR2_SMP18_Pos;                                 /* 640.5 cycles */
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {
    }
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
    uint32_t raw = 0, ref = 0;
    for (int i = 0; i < 2; i++) {
        raw += adc_convert(rail == PDK_RAIL_VDD ? CH_VDD : CH_VPP);
        ref += adc_convert(CH_VREFINT);
    }
    if (!ref)
        return 0;
    /* V = raw / 4095 · VDDA · 6,  VDDA = 3000 mV · VREFINT_CAL / ref */
    uint64_t mv = (uint64_t)raw * VREFINT_CAL_MV * VREFINT_CAL * DIVIDER / ((uint64_t)ref * 4095u);
    return mv > 0xFFFFu ? 0xFFFFu : (uint16_t)mv;
}

void hal_power_off_all(void)
{
    dac_vdd = dac_vpp = 0;
    DAC1->DHR12RD = 0;
}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = VDD_DAC_MAX_MV;
    *vpp_max_mV = VPP_DAC_MAX_MV;
}

#else /* no supply front end */

void g474_supplies_early_off(void) {}
void g474_power_init(void) {}
void hal_power_set(pdk_rail_t rail, uint16_t mV) { (void)rail, (void)mV; }
uint16_t hal_power_measure(pdk_rail_t rail) { (void)rail; return 0; }
void hal_power_off_all(void) {}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = 0;
    *vpp_max_mV = 0;
}

#endif

/* The lines to the PADAUK chip are not assigned yet. */
void platform_ic_lines_hiz(void) {}
