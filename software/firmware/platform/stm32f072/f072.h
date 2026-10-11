/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal interface of the STM32F072 platform (Easy PDK programmer boards).
 *
 * Board pins (from easy-pdk-programmer-software/Firmware, reference only):
 *   PA4 DAC1 → VPP regulator, PA5 DAC2 → VDD regulator, PA2 15 V booster enable,
 *   PA3 TIM15_CH2 50 kHz → −4.8 V charge pump, PB0 ADC_IN8 = VPP/6, PB1 ADC_IN9 = VDD/6
 *   (mini-pill variant: PA0 ADC_IN0 = VPP/6, PA1 ADC_IN1 = VDD/6), PB8/PB9 variant detect,
 *   PB3 IC PA3/CLK, PB4 IC PA4, PB5 IC PA6/DAT, PB6 IC PA0, PB7 IC PA7,
 *   PC13..PC15 LED1..LED3 (active low), PA15 button (pull-down, pressed = 1).
 */
#ifndef PDK_F072_H
#define PDK_F072_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32f0xx.h"

#define F072_HCLK_HZ 48000000u

/* DAC = 0 V on both supplies and IC lines as inputs. Uses no RAM; callable before .data init. */
void f072_power_early_off(void);

void f072_clock_init(void);    /* HSE 8 MHz ×6 = 48 MHz (HSI fallback), HSI48 + CRS for USB */
void f072_time_init(void);     /* SysTick 1 ms, TIM2 1 MHz free-running */
void f072_power_init(void);    /* DAC, ADC, −4.8 V pump, 15 V booster */
void f072_ui_init(void);
void f072_ui_poll(void);       /* button debounce → pdk_local_start() */
void f072_usb_init(void);

#endif
