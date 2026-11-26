/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Clock output for SC-003 (specs/002-chip-engine/quickstart.md §6): the programmer calibrates the IHRC to F_CPU, then
 * the IC toggles one pin every 4 instruction cycles, a square wave of exactly F_CPU / 8 (1.000 MHz at 8 MHz) to measure
 * with a frequency counter or an oscilloscope.
 *
 *   loop: xor  pa, a     1 cycle
 *         nop            1 cycle
 *         goto loop      2 cycles     → half-period 4 cycles, period 8
 *
 * Build: see README.md. Options: -DF_CPU=8000000 (SYSCLK, 1 MHz … 16 MHz IHRC), -DUSE_ILRC (SYSCLK = the ILRC,
 * calibrated to F_CPU, e.g. 55000), -DOUT_PIN=4 (PA bit), -DTARGET_VDD_MV. */
#include <stdint.h>

#include <pdk/device.h>
#include <easy-pdk/calibrate.h>

#ifndef F_CPU
#define F_CPU 8000000
#endif
#ifndef OUT_PIN
#define OUT_PIN 4
#endif
#ifndef TARGET_VDD_MV
#define TARGET_VDD_MV 4000
#endif

#define _S(x) #x
#define S(x) _S(x)

#if __SDCC_REVISION >= 13762
#define STARTUP_FUNCTION __sdcc_external_startup
#else
#define STARTUP_FUNCTION _sdcc_external_startup
#endif

unsigned char STARTUP_FUNCTION(void)
{
#if defined(USE_ILRC)
    PDK_SET_SYSCLOCK(SYSCLOCK_ILRC);
    EASY_PDK_CALIBRATE_ILRC(F_CPU, TARGET_VDD_MV);
#else
#if F_CPU == 16000000
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_16MHZ);
#elif F_CPU == 8000000
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_8MHZ);
#elif F_CPU == 4000000
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_4MHZ);
#elif F_CPU == 2000000
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_2MHZ);
#elif F_CPU == 1000000
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_1MHZ);
#else
#error "F_CPU must be 16, 8, 4, 2 or 1 MHz"
#endif
    EASY_PDK_CALIBRATE_IHRC(F_CPU, TARGET_VDD_MV);   /* the stub the programmer replaces with the measurement loop */
#endif
    return 0;
}

void main(void)
{
    PAC |= (1 << OUT_PIN);   /* output, low */
    PA &= ~(1 << OUT_PIN);
    __asm
        mov a, #(1 << OUT_PIN)
    00001$:
        xor.io __pa, a
        nop
        goto 00001$
    __endasm;
}
