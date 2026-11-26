/* SPDX-License-Identifier: GPL-3.0-or-later */
/* BandGap check (specs/002-chip-engine/quickstart.md §6): the programmer calibrates the IHRC (8 MHz) and the BandGap
 * (the free-pdk EASY_PDK_CALIBRATE_BG stub: BGTR is tuned at VDD 3.95 V until BG ≥ VDD · 12 / 40 ≈ 1.185 V). The
 * program then keeps the comparator exactly as the calibration loop set it — (−) = BandGap, (+) = the internal divider
 * VDD · (N + 9) / 40, range 3, N = 3 — and copies its result to PA4 (PA0, the comparator's own output pin, is the
 * programmer's UART line in RUN):
 *
 *   PA4 = 1  when VDD · 12 / 40 > BG   (VDD above ≈ 3.95 V with a calibrated BandGap)
 *   PA4 = 0  below
 *
 * Sweeping VDD and noting where PA4 flips gives the BandGap voltage: BG = V_flip · 12 / 40. */
#include <stdint.h>

#include <pdk/device.h>
#include <easy-pdk/calibrate.h>

#ifndef OUT_PIN
#define OUT_PIN 4
#endif

#if __SDCC_REVISION >= 13762
#define STARTUP_FUNCTION __sdcc_external_startup
#else
#define STARTUP_FUNCTION _sdcc_external_startup
#endif

unsigned char STARTUP_FUNCTION(void)
{
    PDK_SET_SYSCLOCK(SYSCLOCK_IHRC_8MHZ);
    EASY_PDK_CALIBRATE_IHRC(8000000, 4000);
    EASY_PDK_CALIBRATE_BG();
    return 0;
}

void main(void)
{
    GPCC = (uint8_t)(GPCC_COMP_ENABLE | GPCC_COMP_MINUS_BANDGAP_1V2 | GPCC_COMP_PLUS_VINT_R);   /* 0x84, as in the loop */
    GPCS = (uint8_t)(GPCS_COMP_RANGE3 | 3);                                          /* 0x23: the loop's 0xA3 without PA0 */
    PAC |= (1 << OUT_PIN);
    for (;;) {
        if (GPCC & GPCC_COMP_RESULT_POSITIVE)
            PA |= (1 << OUT_PIN);
        else
            PA &= ~(1 << OUT_PIN);
    }
}
