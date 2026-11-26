/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UART bridge for RUN on the STM32 boards (hal_uart.h): USART1, PB6 = TX → chip PA0, PB7 = RX ← chip PA7
 * (as in the original easy-pdk firmware). The speed comes from the hardware auto baud rate detection on
 * the start bit of the chip's first byte 'U', which is then dropped. Receive and transmit run from the
 * USART interrupt through ring buffers; the RUN loop drains RX. */
#if defined(STM32F072xB) || defined(STM32G474xx)

#include <string.h>

#include "hal_uart.h"

#if defined(STM32F072xB)
#include "f072.h"
#define BRIDGE_PCLK_HZ F072_HCLK_HZ
#define BRIDGE_AF 0u
#define ISR_RXNE USART_ISR_RXNE
#define ISR_TXE USART_ISR_TXE
#define ICR_NOISE USART_ICR_NCF
#define CR1_RXNEIE USART_CR1_RXNEIE
#define CR1_TXEIE USART_CR1_TXEIE
#define GPIOB_CLOCK_ON() (RCC->AHBENR |= RCC_AHBENR_GPIOBEN)
#define LINES_VDD_MAX_MV 0u   /* PB6/PB7 are 5 V tolerant (FT) */
#else
#include "g474.h"
#define BRIDGE_PCLK_HZ G474_HCLK_HZ   /* APB2 = HCLK, USART1 clocked from PCLK2 */
#define BRIDGE_AF 7u
#define ISR_RXNE USART_ISR_RXNE_RXFNE
#define ISR_TXE USART_ISR_TXE_TXFNF
#define ICR_NOISE USART_ICR_NECF
#define CR1_RXNEIE USART_CR1_RXNEIE_RXFNEIE
#define CR1_TXEIE USART_CR1_TXEIE_TXFNFIE
#define GPIOB_CLOCK_ON() (RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN)
#define LINES_VDD_MAX_MV 5000u   /* PB6 is FT_c (UCPD CC line): 5.0 V at most (stm32g474/README.md) */
#endif

#define RING 128u   /* power of two; the client sends at most 64 bytes per UART_TX */
#define ERRORS (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)

static struct {
    volatile bool on;
    volatile uint32_t baud;
    volatile bool first;   /* the next byte is the first one: 'U' is dropped */
    uint8_t rx[RING], tx[RING];
    volatile uint16_t rx_head, rx_tail, tx_head, tx_tail;
} b;

static void pins(bool uart)
{
    const uint32_t m = (3u << (6 * 2)) | (3u << (7 * 2));
    GPIOB->PUPDR &= ~m;
    if (uart) {
        GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFFu << 24)) | (BRIDGE_AF << 24) | (BRIDGE_AF << 28);
        GPIOB->MODER = (GPIOB->MODER & ~m) | (2u << (6 * 2)) | (2u << (7 * 2));   /* alternate function */
    } else {
        GPIOB->MODER &= ~m;   /* inputs: Hi-Z */
    }
}

void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;
    if (isr & ERRORS)
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | ICR_NOISE;
    if (isr & ISR_RXNE) {
        uint8_t c = (uint8_t)USART1->RDR;
        if (!b.baud) {
            if (isr & USART_ISR_ABRE) {   /* detection failed: wait for the next start bit */
                USART1->RQR = USART_RQR_ABRRQ;
                return;
            }
            b.baud = BRIDGE_PCLK_HZ / USART1->BRR;
        }
        if (b.first) {
            b.first = false;
            if (c == 'U')
                return;
        }
        uint16_t next = (uint16_t)((b.rx_head + 1u) & (RING - 1u));
        if (next != b.rx_tail) {   /* full: the byte is lost, as with an overrun */
            b.rx[b.rx_head] = c;
            b.rx_head = next;
        }
    }
    if ((USART1->CR1 & CR1_TXEIE) && (isr & ISR_TXE)) {
        if (b.tx_tail == b.tx_head) {
            USART1->CR1 &= ~CR1_TXEIE;
        } else {
            USART1->TDR = b.tx[b.tx_tail];
            b.tx_tail = (uint16_t)((b.tx_tail + 1u) & (RING - 1u));
        }
    }
}

void hal_uart_start(void)
{
    NVIC_DisableIRQ(USART1_IRQn);
    memset((void *)&b, 0, sizeof b);
    b.first = true;
    GPIOB_CLOCK_ON();
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    USART1->CR1 = 0;
    USART1->BRR = BRIDGE_PCLK_HZ / 115200u;   /* replaced by the detection */
    USART1->CR2 = USART_CR2_ABREN;            /* ABRMOD 00: measure the start bit */
    USART1->CR3 = USART_CR3_OVRDIS;
    USART1->ICR = 0xFFFFFFFFu;
    pins(true);
    USART1->CR1 = CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    b.on = true;
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
}

void hal_uart_stop(void)
{
    NVIC_DisableIRQ(USART1_IRQn);
    USART1->CR1 = 0;
    pins(false);
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;
    b.on = false;
    b.baud = 0;
}

uint32_t hal_uart_baud(void) { return b.on ? b.baud : 0; }

size_t hal_uart_read(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap && b.rx_tail != b.rx_head) {
        buf[n++] = b.rx[b.rx_tail];
        b.rx_tail = (uint16_t)((b.rx_tail + 1u) & (RING - 1u));
    }
    return n;
}

bool hal_uart_write(const uint8_t *data, size_t len)
{
    if (!b.on || !b.baud)
        return false;
    uint16_t used = (uint16_t)((b.tx_head - b.tx_tail) & (RING - 1u));
    if (len > RING - 1u - used)
        return false;
    for (size_t i = 0; i < len; i++) {
        b.tx[b.tx_head] = data[i];
        b.tx_head = (uint16_t)((b.tx_head + 1u) & (RING - 1u));
    }
    USART1->CR1 |= CR1_TXEIE;
    return true;
}

uint16_t hal_uart_vdd_max_mV(void) { return LINES_VDD_MAX_MV; }

#endif
