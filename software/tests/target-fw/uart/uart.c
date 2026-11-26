/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UART bridge check (specs/002-chip-engine/quickstart.md §7): the programmer calibrates the IHRC to 8 MHz; the IC
 * waits ~0.3 s (the programmer starts its UART after VDD has settled), sends 'U' (the programmer's auto-baud
 * character), a greeting, then echoes every byte it receives.
 *
 *   IC TX = PA7 → the programmer, IC RX = PA0 ← the programmer; 8N1, 19200 baud (one bit = 417 cycles at 8 MHz).
 *
 * Bit timing is cycle-counted assembly with balanced branches (dzsn/goto delay: 3·N + 4 cycles with the call):
 *   TX bit: sr, 2 × (test carry, set/clear PA7), delay, dzsn, goto     = 3·N + 12
 *   RX bit: delay, clear carry, test PA0, set carry, src, dzsn, goto   = 3·N + 11 */
#include <stdint.h>

#include <pdk/device.h>
#include <easy-pdk/calibrate.h>

#if __SDCC_REVISION >= 13762
#define STARTUP_FUNCTION __sdcc_external_startup
#else
#define STARTUP_FUNCTION _sdcc_external_startup
#endif

/* N for 19200 baud at 8 MHz: 3·135 + 12 = 417 cycles (416.7 wanted); HALF puts the first sample at ~1.5 bits. */
#define BIT_N 135
#define HALF_N 69

uint8_t uart_b, uart_cnt;

unsigned char STARTUP_FUNCTION(void)
{
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_8MHZ);
    EASY_PDK_CALIBRATE_IHRC(8000000, 4000);
    return 0;
}

/* A = N on entry: 3·N + 1 cycles including the return. */
static void delay_a(void) __naked
{
    __asm
    00001$:
        dzsn a
        goto 00001$
        ret
    __endasm;
}

/* Sends uart_b: start bit, 8 data bits LSB first, stop bit. */
static void uart_tx(void) __naked
{
    __asm
        set0.io __pa, #7
        mov a, #BIT_N
        call _delay_a
        mov a, #8
        mov _uart_cnt+0, a
    00001$:
        sr _uart_b+0
        t0sn.io __flag, #1
        set1.io __pa, #7
        t1sn.io __flag, #1
        set0.io __pa, #7
        mov a, #BIT_N
        call _delay_a
        dzsn _uart_cnt+0
        goto 00001$
        nop
        nop
        set1.io __pa, #7
        mov a, #BIT_N
        call _delay_a
        ret
    __endasm;
}

/* Waits for a start bit and receives one byte into uart_b; returns in the middle of the stop bit. */
static void uart_rx(void) __naked
{
    __asm
    00001$:
        t0sn.io __pa, #0
        goto 00001$
        mov a, #HALF_N
        call _delay_a
        mov a, #8
        mov _uart_cnt+0, a
    00002$:
        mov a, #BIT_N
        call _delay_a
        set0.io __flag, #1
        t0sn.io __pa, #0
        set1.io __flag, #1
        src _uart_b+0
        dzsn _uart_cnt+0
        goto 00002$
        mov a, #BIT_N
        call _delay_a
        ret
    __endasm;
}

static void put(char c)
{
    uart_b = (uint8_t)c;
    uart_tx();
}

static void wait_300ms(void)
{
    /* 230 × 255 × 3·N … ≈ 230 × 41 × 255 cycles ≈ 2.4 M cycles = 0.3 s at 8 MHz */
    for (uint8_t i = 0; i < 230; i++)
        for (uint8_t j = 0; j < 41; j++) {
            __asm
                mov a, #85
                call _delay_a
            __endasm;
        }
}

static const char hello[] = "\r\nPFS154 UART test: type, it echoes\r\n";

void main(void)
{
    PA = 0x80;        /* PA7 idle high */
    PAC = 0x80;       /* PA7 output, PA0 input */
    PAPH |= 0x01;     /* PA0 pull-up: idle high without the programmer */
    PADIER |= 0x81;   /* PA0, PA7 digital */
    wait_300ms();
    put('U');
    for (const char *p = hello; *p; p++)
        put(*p);
    for (;;) {
        uart_rx();
        uart_tx();    /* echo: uart_b still holds the byte */
    }
}
