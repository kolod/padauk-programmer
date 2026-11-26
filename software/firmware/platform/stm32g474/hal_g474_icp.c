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
#include "hal_time.h"

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

/* ---- calibration counter (research R9) ----
 *
 * "+1" pulses: SPI3 in slave mode, transmit-only (bidirectional, output), SCK = PB3 ← the IC's PA3, MISO = PC11 → the IC's
 * PA4, MSB first, CPOL 0 / CPHA 0 — MISO changes on the falling edge and is stable while PA3 is high, when the loop samples
 * PA4 (T0SN PA.4). Circular DMA streams a 256-byte buffer of zeros: one SPI bit = one loop pass, so writing n one-bits
 * ahead of the DMA position gives exactly n steps; they are cleared once the DMA has fetched them. 256 bytes = 2048 passes
 * (≥ 0.9 ms even at 16 MHz) leave room for interrupts between fetching and clearing.
 * Frequency: TIM2 in external clock mode 1 on TI1 = PA15 (the same PA3 line through its own resistor, input only) counts
 * PA3 rising edges; the window is timed by the DWT cycle counter. */

#define PULSE_BUF 256u
#define PULSE_AHEAD 8u      /* bytes ahead of the DMA position (the SPI TX FIFO holds 4) */
#define DMAMUX_REQ_SPI3_TX 15u

static uint8_t pulse_buf[PULSE_BUF];
static bool calib_on;

static void af(GPIO_TypeDef *p, unsigned pin, unsigned fn)
{
    unsigned sh = 2u * pin;
    volatile uint32_t *afr = &p->AFR[pin >> 3];
    *afr = (*afr & ~(0xFu << (4u * (pin & 7u)))) | (fn << (4u * (pin & 7u)));
    p->PUPDR &= ~(3u << sh);
    p->MODER = (p->MODER & ~(3u << sh)) | (2u << sh);
}

static unsigned dma_pos(void) { return (PULSE_BUF - DMA1_Channel1->CNDTR) % PULSE_BUF; }

bool hal_icp_calib_begin(void)
{
    clocks_on();
    RCC->APB1ENR1 |= RCC_APB1ENR1_SPI3EN | RCC_APB1ENR1_TIM2EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
    (void)RCC->AHB1ENR;
    for (unsigned i = 0; i < PULSE_BUF; i++)
        pulse_buf[i] = 0;

    /* TIM2: count PA15 (TI1FP1) rising edges */
    TIM2->CR1 = 0;
    TIM2->PSC = 0;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->CCMR1 = TIM_CCMR1_CC1S_0 | (2u << TIM_CCMR1_IC1F_Pos);   /* TI1 input, a short digital filter */
    TIM2->CCER = 0;                                                  /* rising edge */
    TIM2->SMCR = TIM_SMCR_SMS_0 | TIM_SMCR_SMS_1 | TIM_SMCR_SMS_2 | TIM_SMCR_TS_0 | TIM_SMCR_TS_2;   /* ext. clock, TI1FP1 */
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 = TIM_CR1_CEN;
    af(GPIOA, 15, 1);   /* TIM2_CH1: an input in this mode */

    /* SPI3 slave, transmit-only, 8-bit, TX by circular DMA */
    SPI3->CR1 = 0;
    SPI3->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH | SPI_CR2_TXDMAEN;
    SPI3->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_SSM;   /* slave, NSS by software (selected), MSB first */
    DMA1_Channel1->CCR = 0;
    DMAMUX1_Channel0->CCR = DMAMUX_REQ_SPI3_TX;
    DMA1_Channel1->CPAR = (uint32_t)&SPI3->DR;
    DMA1_Channel1->CMAR = (uint32_t)pulse_buf;
    DMA1_Channel1->CNDTR = PULSE_BUF;
    DMA1_Channel1->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_DIR | DMA_CCR_EN;   /* 8-bit memory → peripheral */
    af(GPIOB, 3, 6);    /* SPI3_SCK: the IC drives PA3 */
    af(GPIOC, 11, 6);   /* SPI3_MISO → the IC's PA4 */
    SPI3->CR1 |= SPI_CR1_SPE;
    calib_on = true;
    return true;
}

bool hal_icp_calib_pulses(uint16_t n, uint32_t timeout_us)
{
    if (!calib_on || !n || n > 8u * PULSE_AHEAD)
        return false;
    unsigned bytes = (n + 7u) / 8u, start = (dma_pos() + PULSE_AHEAD) % PULSE_BUF;
    for (unsigned i = 0; i < bytes; i++) {   /* MSB first: the last byte carries its ones in the top bits */
        unsigned left = n - 8u * i;
        pulse_buf[(start + i) % PULSE_BUF] = left >= 8u ? 0xFFu : (uint8_t)(0xFFu << (8u - left));
    }
    /* wait until the DMA has fetched them: the position moves PULSE_AHEAD + bytes on */
    unsigned need = PULSE_AHEAD + bytes, moved = 0, last = dma_pos();
    uint32_t t0 = hal_time_us();
    bool ok = true;
    while (moved < need) {
        unsigned now = dma_pos();
        moved += (now + PULSE_BUF - last) % PULSE_BUF;
        last = now;
        if (hal_time_us() - t0 > timeout_us) {
            ok = false;   /* the IC is not clocking: no loop running */
            break;
        }
    }
    for (unsigned i = 0; i < bytes; i++)
        pulse_buf[(start + i) % PULSE_BUF] = 0;
    return ok;
}

uint32_t hal_icp_calib_count(uint32_t window_us, uint32_t *elapsed_us)
{
    uint32_t cycles = window_us * (G474_HCLK_HZ / 1000000u);
    uint32_t c0 = TIM2->CNT, t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < cycles) {
    }
    uint32_t c1 = TIM2->CNT, dt = DWT->CYCCNT - t0;
    *elapsed_us = dt / (G474_HCLK_HZ / 1000000u);
    return c1 - c0;
}

void hal_icp_calib_end(void)
{
    if (!calib_on)
        return;
    calib_on = false;
    SPI3->CR1 &= ~SPI_CR1_SPE;
    DMA1_Channel1->CCR = 0;
    TIM2->CR1 = 0;
    GPIOB->MODER &= ~(3u << (2u * 3u));     /* PB3, PC11, PA15 back to inputs; hal_icp_idle sets the pulls */
    GPIOC->MODER &= ~(3u << (2u * 11u));
    GPIOA->MODER &= ~(3u << (2u * 15u));
    RCC->APB1ENR1 &= ~(RCC_APB1ENR1_SPI3EN | RCC_APB1ENR1_TIM2EN);
}
