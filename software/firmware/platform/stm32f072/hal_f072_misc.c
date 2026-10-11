/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Time, CRC unit, entropy, UID, platform name and the bring-up UI (3 LEDs, 1 button). */
#include "f072.h"
#include "hal_crc.h"
#include "hal_entropy.h"
#include "hal_platform.h"
#include "hal_time.h"
#include "hal_ui.h"

/* ---------------- time: SysTick 1 ms + TIM2 (32 bit) at 1 MHz ---------------- */
static volatile uint32_t ms_ticks;

void SysTick_Handler(void) { ms_ticks++; }

void f072_time_init(void)
{
    SysTick->LOAD = F072_HCLK_HZ / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->PSC = F072_HCLK_HZ / 1000000u - 1u;
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

/* ---------------- CRC unit ----------------
 * The core keeps the reflected running state (zlib convention, no final xor). The unit works
 * MSB-first, so its register holds the bit-reversed state: load INIT = rbit(state), feed bytes with
 * input reversal, read DR with output reversal = new reflected state. */
static uint32_t rbit32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len)
{
    static bool on;
    if (!on) {
        RCC->AHBENR |= RCC_AHBENR_CRCEN;
        on = true;
    }
    const uint8_t *p = data;
    CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT;   /* byte-wise input reversal, default polynomial */
    CRC->INIT = rbit32(state);
    CRC->CR |= CRC_CR_RESET;
    while (len--)
        *(volatile uint8_t *)&CRC->DR = *p++;
    return CRC->DR;
}

/* ---------------- platform ---------------- */
const char *hal_platform_name(void) { return "stm32f072c8"; }

void hal_uid(uint8_t uid[12])
{
    const uint8_t *src = (const uint8_t *)UID_BASE;
    for (int i = 0; i < 12; i++)
        uid[i] = src[i];
}

/* ---------------- entropy: LSBs of temperature-sensor conversions ----------------
 * Uses the ADC owned by hal_f072_power.c. Rejects a stuck source (all LSBs equal). */
uint16_t f072_adc_convert(uint32_t channel);

bool hal_entropy_raw(uint8_t *buf, size_t len)
{
    uint32_t ones = 0, total = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            uint16_t a = f072_adc_convert(16);   /* temperature sensor */
            uint16_t c = f072_adc_convert(17);   /* VREFINT */
            uint8_t bit = (uint8_t)((a ^ c ^ (a >> 1)) & 1u);
            b = (uint8_t)(b << 1 | bit);
            ones += bit;
            total++;
        }
        buf[i] = b;
    }
    return total < 64 || (ones > total / 8 && ones < total - total / 8);
}

/* ---------------- UI: LED1 PC13, LED2 PC14, LED3 PC15 (active low); button PA15 ---------------- */
static void leds(uint8_t mask)
{
    uint32_t set = 0, reset = 0;
    for (int i = 0; i < 3; i++) {
        if (mask & (1u << i))
            reset |= 1u << (13 + i);   /* on = low */
        else
            set |= 1u << (13 + i);
    }
    GPIOC->BSRR = set | (reset << 16);
}

void f072_ui_init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOCEN;
    GPIOC->BSRR = (7u << 13);   /* LEDs off */
    GPIOC->MODER = (GPIOC->MODER & ~(0x3Fu << 26)) | (0x15u << 26);
    GPIOA->MODER &= ~(3u << 30);                                 /* PA15 input */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 30)) | (2u << 30);    /* pull-down */
    leds(1);
}

/* Bring-up mapping (final indication: T102). */
void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *detail)
{
    (void)detail;
    switch (state) {
    case PDK_UI_READY: leds(1); break;
    case PDK_UI_BUSY: leds(2); break;
    case PDK_UI_SUCCESS: leds(3); break;
    case PDK_UI_FAIL: leds(4); break;
    case PDK_UI_BLOCKED: leds(5); break;
    }
}

void f072_ui_poll(void)
{
    static uint32_t since;
    static bool stable, last;
    bool now = (GPIOA->IDR & (1u << 15)) != 0;
    uint32_t t = hal_time_ms();
    if (now != last) {
        last = now;
        since = t;
    } else if (now != stable && t - since >= 30u) {
        stable = now;
        if (stable)
            pdk_local_start(PDK_LOCAL_PROGRAM);
    }
}
