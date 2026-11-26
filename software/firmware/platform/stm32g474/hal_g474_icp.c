/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PADAUK programming lines on the STM32G474 board (hal_icp.h, specs/002-chip-engine/contracts/hal-icp.md).
 *
 *   IC PA3 / ICPCK  PB3  (GPIO; SPI3_SCK during calibration)   + PA15 (TIM2_CH1 capture, input only — never driven)
 *   IC PA6 / ICPDA  PB5
 *   IC PA4          PC11 (GPIO; SPI3_MISO during calibration)
 *   IC PA0          PB6  (FT_c: 5.0 V at most — see hal_icp_vdd_max_mV)
 *   IC PA7          PB7
 *
 * Bit timing: the clock-high of each bit is at least 1 µs (DWT cycle counter at 170 MHz); the data line is set at least
 * 200 ns before the clock rises and the clock stays low at least 200 ns, so the edges are clean through the 100 Ω series
 * resistors. All durations are minimums: an interrupt may stretch a level. */
#include "g474.h"
#include "hal_icp.h"

#define SETUP_NS 200u

typedef struct {
    GPIO_TypeDef *port;
    uint8_t pin;
} pin_t;

static const pin_t lines[ICP_LINES] = {
    [ICP_CLK] = {GPIOB, 3}, [ICP_DAT] = {GPIOB, 5}, [ICP_PA4] = {GPIOC, 11}, [ICP_PA0] = {GPIOB, 6}, [ICP_PA7] = {GPIOB, 7},
};

static void clocks_on(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;
}

void hal_icp_delay_ns(uint32_t ns)
{
    uint32_t cycles = (uint32_t)(((uint64_t)ns * (G474_HCLK_HZ / 1000000u) + 999u) / 1000u);
    uint32_t t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < cycles) {
    }
}

static inline void level(icp_line_t l, bool high)
{
    lines[l].port->BSRR = high ? (1u << lines[l].pin) : (1u << (lines[l].pin + 16u));
}

void hal_icp_mode(icp_line_t l, icp_dir_t dir)
{
    GPIO_TypeDef *p = lines[l].port;
    unsigned pin = lines[l].pin, sh = 2u * pin;
    p->OTYPER &= ~(1u << pin);
    p->OSPEEDR = (p->OSPEEDR & ~(3u << sh)) | (1u << sh);   /* medium speed: ~10 ns edges are plenty */
    switch (dir) {
    case ICP_OUT_LOW:
    case ICP_OUT_HIGH:
        level(l, dir == ICP_OUT_HIGH);   /* the level first, then the output: no glitch */
        p->PUPDR &= ~(3u << sh);
        p->MODER = (p->MODER & ~(3u << sh)) | (1u << sh);
        break;
    default:
        p->MODER &= ~(3u << sh);
        p->PUPDR = (p->PUPDR & ~(3u << sh)) |
                   ((dir == ICP_IN_PULLDOWN ? 2u : dir == ICP_IN_PULLUP ? 1u : 0u) << sh);
        break;
    }
}

void hal_icp_idle(void)
{
    clocks_on();
    hal_icp_calib_end();
    GPIOA->MODER &= ~(3u << (2u * 15u));   /* PA15: the capture input, never an output */
    GPIOA->PUPDR &= ~(3u << (2u * 15u));
    for (unsigned l = 0; l < ICP_LINES; l++)
        hal_icp_mode((icp_line_t)l, (l == ICP_CLK || l == ICP_DAT) ? ICP_IN_PULLDOWN : ICP_IN);
}

bool hal_icp_level(icp_line_t l) { return (lines[l].port->IDR >> lines[l].pin) & 1u; }

void hal_icp_clock(uint32_t high_ns)
{
    level(ICP_CLK, true);
    hal_icp_delay_ns(high_ns);
    level(ICP_CLK, false);
    hal_icp_delay_ns(SETUP_NS);
}

void hal_icp_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits)
{
    while (bits--) {
        level(data, (value >> bits) & 1u);
        hal_icp_delay_ns(SETUP_NS);
        level(clk, true);
        hal_icp_delay_ns(1000u);
        level(clk, false);
    }
    hal_icp_delay_ns(SETUP_NS);
}

uint32_t hal_icp_recv(icp_line_t data, icp_line_t clk, uint8_t bits)
{
    uint32_t v = 0;
    while (bits--) {
        level(clk, true);
        hal_icp_delay_ns(1000u);
        v = v << 1 | (uint32_t)hal_icp_level(data);
        level(clk, false);
        hal_icp_delay_ns(SETUP_NS);
    }
    return v;
}

/* The IC drives PA0 into PB6 (FT_c) while its program runs (calibration, RUN). */
uint16_t hal_icp_vdd_max_mV(void) { return 5000u; }

/* ---- calibration counter: SPI3 slave + TIM2 on PA15 (user story 3) ---- */

bool hal_icp_calib_begin(void) { return false; }
bool hal_icp_calib_pulses(uint16_t n, uint32_t timeout_us)
{
    (void)n, (void)timeout_us;
    return false;
}
uint32_t hal_icp_calib_count(uint32_t window_us, uint32_t *elapsed_us)
{
    *elapsed_us = window_us;
    return 0;
}
void hal_icp_calib_end(void) {}
