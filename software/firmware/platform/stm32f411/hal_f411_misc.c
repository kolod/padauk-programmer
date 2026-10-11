/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Time, CRC, entropy, UID, platform name, single-LED UI with the KEY button, USB OTG FS. */
#include "f411.h"
#include "hal_crc.h"
#include "hal_entropy.h"
#include "hal_platform.h"
#include "hal_time.h"
#include "hal_ui.h"
#include "proto/crc32.h"
#include "tusb.h"

/* ---------------- time: SysTick 1 ms + TIM2 (32 bit) at 1 MHz ---------------- */
static volatile uint32_t ms_ticks;

void SysTick_Handler(void) { ms_ticks++; }

void f411_time_init(void)
{
    SysTick->LOAD = F411_HCLK_HZ / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->PSC = 2u * F411_APB1_HZ / 1000000u - 1u;   /* APB1 timers run at 2 × PCLK1 */
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 = TIM_CR1_CEN;
}

uint32_t hal_time_ms(void) { return ms_ticks; }
uint32_t hal_time_us(void) { return TIM2->CNT; }

void hal_delay_us(uint32_t us)
{
    uint32_t t0 = TIM2->CNT;
    while (TIM2->CNT - t0 < us) {
    }
}

/* The F4 CRC unit has no bit reversal and works on 32-bit words only: use the software CRC. */
uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len) { return crc32_sw_update(state, data, len); }

const char *hal_platform_name(void) { return "stm32f411ce"; }

void hal_uid(uint8_t uid[12])
{
    const uint8_t *src = (const uint8_t *)UID_BASE;
    for (int i = 0; i < 12; i++)
        uid[i] = src[i];
}

/* ---------------- ADC1: temperature sensor (IN18) and VREFINT (IN17) for entropy ---------------- */
#define ADC_CH_VREFINT 17u
#define ADC_CH_TEMP 18u

void f411_adc_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0 | ADC_CCR_TSVREFE;   /* PCLK2 / 4 = 24 MHz */
    ADC1->SMPR1 = (7u << ADC_SMPR1_SMP17_Pos) | (7u << ADC_SMPR1_SMP18_Pos);      /* 480 cycles */
    ADC1->CR2 = ADC_CR2_ADON;
    hal_delay_us(20);
}

uint16_t f411_adc_convert(uint32_t channel)
{
    ADC1->SQR3 = channel;
    ADC1->SR = 0;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {
    }
    return (uint16_t)ADC1->DR;
}

/* LSBs of noisy conversions; rejects a stuck source (all bits equal or nearly so). */
bool hal_entropy_raw(uint8_t *buf, size_t len)
{
    uint32_t ones = 0, total = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            uint16_t a = f411_adc_convert(ADC_CH_TEMP);
            uint16_t c = f411_adc_convert(ADC_CH_VREFINT);
            uint8_t bit = (uint8_t)((a ^ c ^ (a >> 1)) & 1u);
            b = (uint8_t)(b << 1 | bit);
            ones += bit;
            total++;
        }
        buf[i] = b;
    }
    return total < 64 || (ones > total / 8 && ones < total - total / 8);
}

/* ---------------- UI: one LED (PC13, active low) shows the state by its pattern ----------------
 *   READY steady on · BUSY 4 Hz · SUCCESS 1 Hz · FAIL 10 Hz · BLOCKED off
 * KEY (PA0, pressed = 0) → local PROGRAM. Final indication comes with T102. */
static volatile pdk_ui_state_t ui_state = PDK_UI_READY;

static void led(bool on) { GPIOC->BSRR = on ? (1u << (13 + 16)) : (1u << 13); }

void f411_ui_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN;
    led(false);
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 26)) | (1u << 26);
    GPIOA->MODER &= ~3u;                          /* PA0 input */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~3u) | 1u;     /* pull-up */
}

void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *detail)
{
    (void)detail;
    ui_state = state;
}

void f411_ui_poll(void)
{
    uint32_t t = hal_time_ms();
    switch (ui_state) {
    case PDK_UI_READY: led(true); break;
    case PDK_UI_BUSY: led((t / 125u) & 1u); break;
    case PDK_UI_SUCCESS: led((t / 500u) & 1u); break;
    case PDK_UI_FAIL: led((t / 50u) & 1u); break;
    case PDK_UI_BLOCKED: led(false); break;
    }

    static uint32_t since;
    static bool stable, last;
    bool now = !(GPIOA->IDR & 1u);
    if (now != last) {
        last = now;
        since = t;
    } else if (now != stable && t - since >= 30u) {
        stable = now;
        if (stable)
            pdk_local_start(PDK_LOCAL_PROGRAM);
    }
}

/* ---------------- USB OTG FS on PA11/PA12 ---------------- */
void OTG_FS_IRQHandler(void) { tud_int_handler(0); }

static void no_vbus_sensing(void)
{
    USB_OTG_FS->GCCFG &= ~(USB_OTG_GCCFG_VBUSASEN | USB_OTG_GCCFG_VBUSBSEN);
    USB_OTG_FS->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
}

void platform_usb_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER = (GPIOA->MODER & ~(0xFu << 22)) | (0xAu << 22);    /* PA11, PA12 alternate */
    GPIOA->OSPEEDR |= 0xFu << 22;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFFu << 12)) | (0xAAu << 12); /* AF10 OTG_FS */
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    no_vbus_sensing();
    tud_init(0);
    no_vbus_sensing();   /* the core reset in tud_init() must not bring it back */
}
