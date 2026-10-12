/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Time, CRC unit, true RNG, UID, platform name, single-LED UI with the KEY button, USB FS. */
#include "g474.h"
#include "hal_crc.h"
#include "hal_entropy.h"
#include "hal_platform.h"
#include "hal_time.h"
#include "hal_ui.h"
#include "tusb.h"

/* ---------------- time: SysTick 1 ms + TIM2 (32 bit) at 1 MHz ---------------- */
static volatile uint32_t ms_ticks;

void SysTick_Handler(void) { ms_ticks++; }

void g474_time_init(void)
{
    SysTick->LOAD = G474_HCLK_HZ / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
    TIM2->PSC = G474_HCLK_HZ / 1000000u - 1u;   /* APB1 = HCLK: timer clock 170 MHz */
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

/* ---------------- CRC unit (same scheme as on the F072) ----------------
 * The core keeps the reflected running state; the unit works MSB-first: INIT = rbit(state), input
 * reversed per byte, output reversed. */
uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len)
{
    static bool on;
    if (!on) {
        RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
        (void)RCC->AHB1ENR;
        on = true;
    }
    const uint8_t *p = data;
    CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT;
    CRC->INIT = __RBIT(state);
    CRC->CR |= CRC_CR_RESET;
    while (len--)
        *(volatile uint8_t *)&CRC->DR = *p++;
    return CRC->DR;
}

/* ---------------- platform ---------------- */
const char *hal_platform_name(void) { return "stm32g474ce"; }

void hal_uid(uint8_t uid[12])
{
    const uint8_t *src = (const uint8_t *)UID_BASE;
    for (int i = 0; i < 12; i++)
        uid[i] = src[i];
}

/* ---------------- entropy: the on-chip true RNG (48 MHz from HSI48) ---------------- */
bool hal_entropy_raw(uint8_t *buf, size_t len)
{
    static bool on;
    if (!on) {
        RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
        (void)RCC->AHB2ENR;
        RNG->CR = RNG_CR_RNGEN;
        on = true;
    }
    for (size_t i = 0; i < len;) {
        uint32_t t0 = hal_time_ms();
        while (!(RNG->SR & RNG_SR_DRDY)) {
            if ((RNG->SR & (RNG_SR_SECS | RNG_SR_CECS)) || hal_time_ms() - t0 > 10u)
                return false;   /* seed or clock error: the source is not trusted */
        }
        uint32_t v = RNG->DR;
        if (!v)
            return false;       /* DR = 0 after a seed error */
        for (int k = 0; k < 4 && i < len; k++, i++)
            buf[i] = (uint8_t)(v >> (8 * k));
    }
    return true;
}

/* ---------------- UI: one LED (PC6, active high) shows the state by its pattern ----------------
 *   READY steady on · BUSY 4 Hz · SUCCESS 1 Hz · FAIL 10 Hz · BLOCKED off
 * KEY (PC13, pressed = 1) → local PROGRAM. Final indication comes with T102. */
static volatile pdk_ui_state_t ui_state = PDK_UI_READY;

static void led(bool on) { GPIOC->BSRR = on ? (1u << 6) : (1u << (6 + 16)); }

void g474_ui_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;
    led(false);
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 12)) | (1u << 12);    /* PC6 output */
    GPIOC->MODER &= ~(3u << 26);                                 /* PC13 input */
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(3u << 26)) | (2u << 26);    /* pull-down */
}

void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *detail)
{
    (void)detail;
    ui_state = state;
}

void g474_ui_poll(void)
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
    bool now = (GPIOC->IDR & (1u << 13)) != 0;
    if (now != last) {
        last = now;
        since = t;
    } else if (now != stable && t - since >= 30u) {
        stable = now;
        if (stable)
            pdk_local_start(PDK_LOCAL_PROGRAM);
    }
}

/* ---------------- USB FS (PA11/PA12 are taken over by the peripheral) ---------------- */
void USB_HP_IRQHandler(void) { tud_int_handler(0); }
void USB_LP_IRQHandler(void) { tud_int_handler(0); }
void USBWakeUp_IRQHandler(void) { tud_int_handler(0); }

void platform_usb_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_USBEN;
    (void)RCC->APB1ENR1;
    tud_init(0);
}
