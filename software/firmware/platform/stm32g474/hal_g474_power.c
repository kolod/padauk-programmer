/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supplies of the G474 platform (provisional front end, see g474.h and README.md).
 *
 * Rails for the output stage, made without dedicated ICs:
 *   +15 V  boost converter (logic-level N-MOSFET, inductor, Schottky) switched by TIM1_CH1 (PA8).
 *          Regulation is hysteretic and entirely in hardware: COMP4 compares the divided output
 *          (PB0, /6) with an internal DAC3_CH2 threshold and clears the PWM (OCREF_CLR) while the
 *          output is above it. Firmware only sets the threshold (with a soft start).
 *          Overvoltage: COMP1 compares a second, independent divider (PB1, /14) with VREFINT and
 *          trips TIM1 break: the gate goes low and stays low until reset (AOE = 0). COMP1 is locked.
 *   -2.5 V diode charge pump driven by TIM16_CH1 (PB4), 100 kHz, 50 %, low-speed edges. PB4 is on the
 *          far side of the package from the analog pins (PA0…PB2, VREF+, VDDA).
 * VDD/VPP: DAC1 (output buffers off, so 0 V is reachable) → linear output stage (op-amp from
 *   +15 V / -2.5 V), as on the Easy PDK board:
 *   DAC full scale → 6.26 V VDD / 13.27 V VPP, ADC inputs on /6 dividers, VDDA from VREFINT
 *   (calibrated at 3.0 V on the G4).
 * Without PDK_G474_SUPPLIES nothing is driven: limits are 0 V, measurements 0 V, and every nonzero
 * set point ends in VDD_FAULT/VPP_FAULT. */
#include "g474.h"
#include "hal_power.h"
#include "hal_time.h"

#ifdef PDK_G474_SUPPLIES

#define VDD_DAC_MAX_MV (6290u - 30u)
#define VPP_DAC_MAX_MV (13300u - 30u)
#define DIVIDER 6u
#define VREFINT_CAL (*(const uint16_t *)0x1FFF75AAu)   /* VREFINT at VDDA = 3.0 V, factory */
#define VREFINT_CAL_MV 3000u
#define CH_VDD 1u    /* PA0 */
#define CH_VPP 2u    /* PA1 */
#define CH_VREFINT 18u

#define BOOST_MV 15000u            /* +15 V rail */
#define BOOST_DIVIDER 6u           /* PB0 = +15 V / 6 (e.g. 100 k / 20 k) */
#define BOOST_PWM_HZ 400000u
#define BOOST_MAX_DUTY_PCT 75u     /* also bounds the inductor current */
#define BOOST_SOFTSTART_MS 20u
#define BOOST_READY_TIMEOUT_MS 50u
#define PUMP_PWM_HZ 100000u        /* -2.5 V charge pump */
/* Overvoltage trip: PB1 = +15 V / 14 (e.g. 130 k / 10 k) against VREFINT (1.21 V typ) → ≈ 17 V. */

static uint16_t dac_vdd, dac_vpp;
static bool rails_ok;

/* Gate low and boost stopped, DAC = 0 V. No RAM state: callable before .data init and from faults. */
void g474_supplies_early_off(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_DAC1EN;
    (void)RCC->AHB2ENR;
    GPIOA->BSRR = 1u << (8 + 16);                                  /* PA8 (boost gate) low */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (1u << 16);      /* PA8 output */
    if (RCC->APB2ENR & RCC_APB2ENR_TIM1EN)
        TIM1->BDTR &= ~TIM_BDTR_MOE;
    GPIOA->MODER |= (3u << 8) | (3u << 10);   /* PA4, PA5 analog (reset state) */
    /* Output buffers off (MODE = 010, external pin): the load is an op-amp input with a 240 k pull-down,
     * and only the unbuffered DAC reaches 0 V (buffered: about 0.2 V … VDDA − 0.2 V). The output
     * impedance (~10-15 k) and the 10 nF on the pin form the anti-noise RC filter. */
    DAC1->MCR = DAC_MCR_HFSEL_1 | (2u << DAC_MCR_MODE1_Pos) | (2u << DAC_MCR_MODE2_Pos);
    DAC1->DHR12RD = 0;
    DAC1->CR = DAC_CR_EN1 | DAC_CR_EN2;
}

static uint32_t mv_to_dac(uint32_t mv_at_pin) { return mv_at_pin * 4095u / 3300u; }   /* VDDA = 3.3 V */

/* Brings up the -2.5 V and +15 V rails; false if +15 V does not reach its threshold. */
static bool rails_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN | RCC_APB2ENR_TIM1EN | RCC_APB2ENR_TIM16EN;
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_DAC3EN;
    (void)RCC->AHB2ENR;
    GPIOB->MODER |= (3u << 0) | (3u << 2);   /* PB0, PB1 analog */

    /* overvoltage comparator first, then locked until reset */
    COMP1->CSR = COMP_CSR_INPSEL /* INP1 = PB1 */ | (3u << COMP_CSR_INMSEL_Pos) /* VREFINT */ | COMP_CSR_SCALEN |
                 (2u << COMP_CSR_HYST_Pos);
    COMP1->CSR |= COMP_CSR_EN;
    hal_delay_us(10);
    COMP1->CSR |= COMP_CSR_LOCK;

    /* -2.5 V charge pump: TIM16_CH1 on PB4 (AF1), low speed, no pull (NJTRST pull-up off) */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFu << 16)) | (1u << 16);
    GPIOB->OSPEEDR &= ~(3u << 8);
    GPIOB->PUPDR &= ~(3u << 8);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 8)) | (2u << 8);
    TIM16->PSC = 0;
    TIM16->ARR = G474_HCLK_HZ / PUMP_PWM_HZ - 1u;
    TIM16->CCR1 = (TIM16->ARR + 1u) / 2u;
    TIM16->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM16->CCER = TIM_CCER_CC1E;
    TIM16->BDTR = TIM_BDTR_MOE;
    TIM16->EGR = TIM_EGR_UG;
    TIM16->CR1 = TIM_CR1_CEN;
    hal_delay_us(5000);

    /* regulation threshold: DAC3 channel 2, internal only */
    DAC3->MCR = DAC_MCR_HFSEL_1 | (3u << DAC_MCR_MODE2_Pos);   /* on-chip peripherals, no buffer */
    DAC3->DHR12R2 = 0;
    DAC3->CR = DAC_CR_EN2;
    hal_delay_us(10);
    COMP4->CSR = (4u << COMP_CSR_INMSEL_Pos) /* DAC3_CH2 */ | (2u << COMP_CSR_HYST_Pos);   /* INP0 = PB0 */
    COMP4->CSR |= COMP_CSR_EN;

    /* boost PWM: TIM1_CH1 on PA8 (AF6); COMP4 clears it, COMP1 breaks it */
    TIM1->PSC = 0;
    TIM1->ARR = G474_HCLK_HZ / BOOST_PWM_HZ - 1u;
    TIM1->CCR1 = (TIM1->ARR + 1u) * BOOST_MAX_DUTY_PCT / 100u;
    TIM1->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1CE;
    TIM1->SMCR &= ~TIM_SMCR_OCCS;                               /* clear from ocref_clr_int */
    TIM1->AF2 = (TIM1->AF2 & ~TIM1_AF2_OCRSEL) | (3u << TIM1_AF2_OCRSEL_Pos);   /* COMP4 */
    TIM1->AF1 = TIM1_AF1_BKCMP1E;                               /* break from COMP1 only, active high */
    TIM1->CCER = TIM_CCER_CC1E;
    TIM1->BDTR = TIM_BDTR_BKE | TIM_BDTR_BKP | TIM_BDTR_OSSI | TIM_BDTR_OSSR;   /* AOE = 0: latched */
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~0xFu) | 6u;
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << 16)) | (1u << 16);   /* medium: edges set by the gate resistor */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (2u << 16);
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    TIM1->BDTR |= TIM_BDTR_MOE;

    /* soft start: raise the threshold from 0 to the target */
    uint32_t target = mv_to_dac(BOOST_MV / BOOST_DIVIDER);
    for (uint32_t ms = 1; ms <= BOOST_SOFTSTART_MS; ms++) {
        DAC3->DHR12R2 = target * ms / BOOST_SOFTSTART_MS;
        hal_delay_us(1000);
    }
    uint32_t t0 = hal_time_ms();
    while (!(COMP4->CSR & COMP_CSR_VALUE)) {   /* output above the threshold at least once */
        if (hal_time_ms() - t0 > BOOST_READY_TIMEOUT_MS || !(TIM1->BDTR & TIM_BDTR_MOE)) {
            TIM1->BDTR &= ~TIM_BDTR_MOE;
            return false;
        }
    }
    return (TIM1->BDTR & TIM_BDTR_MOE) != 0;   /* no overvoltage trip */
}

/* +15 V is up and has not tripped the overvoltage break since. */
static bool rails_up(void) { return rails_ok && (TIM1->BDTR & TIM_BDTR_MOE) && !(TIM1->SR & TIM_SR_BIF); }

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
    rails_ok = rails_init();
}

void hal_power_set(pdk_rail_t rail, uint16_t mV)
{
    uint32_t max = rail == PDK_RAIL_VDD ? VDD_DAC_MAX_MV : VPP_DAC_MAX_MV;
    uint32_t v = (uint32_t)mV * 4095u / max;
    if (v > 4095u)
        v = 4095u;
    if (!rails_up())
        v = 0;   /* no output stage supply: keep the references at 0 V */
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
    bool up = rails_up();   /* a dead +15 V rail is reported as "no supply front end" */
    *vdd_max_mV = up ? VDD_DAC_MAX_MV : 0;
    *vpp_max_mV = up ? VPP_DAC_MAX_MV : 0;
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
