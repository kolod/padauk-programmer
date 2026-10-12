/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal interface of the STM32G474CEU6 platform: WeAct Studio STM32G474 core board
 * (https://github.com/WeActStudio/WeActStudio.STM32G474CoreBoard), no external flash.
 *
 * Board (from the WeAct examples): HSE 8 MHz, LSE 32.768 kHz; LED PC6 (active high, CEU6 variant);
 * KEY PC13 (pull-down, pressed = 1); USB FS on PA11/PA12, clocked from HSI48 + CRS.
 *
 * Storage: packages and the state journal live in flash bank 2 (0x08040000…), the firmware runs from
 * bank 1, so erasing and programming bank 2 does not stall the CPU (read-while-write, DBANK = 1).
 *
 * Supplies (provisional pins, only with -DPDK_G474_SUPPLIES=ON): DAC1_OUT1 PA4 → VPP regulator,
 * DAC1_OUT2 PA5 → VDD regulator, ADC1_IN1 PA0 = VDD/6, ADC1_IN2 PA1 = VPP/6 (Easy PDK scaling);
 * +15 V boost: TIM1_CH1 PA8 gate, COMP4_INP PB0 = +15 V/6 (regulation), COMP1_INP PB1 = +15 V/14
 * (overvoltage); -2.5 V charge pump: TIM16_CH1 PB4.
 * Without the option the board has no supplies: limits are 0 V and INFO hides MEASURE_POWER.
 */
#ifndef PDK_G474_H
#define PDK_G474_H

#include <stdbool.h>
#include <stdint.h>

#include "platform_common.h"
#include "stm32g4xx.h"

#define G474_HCLK_HZ 170000000u   /* PLL: 8 MHz / 2 × 85 / 2, range 1 boost */

void g474_supplies_early_off(void);   /* DAC = 0 V; no RAM state, callable before .data init */
void g474_clock_init(void);
void g474_time_init(void);            /* SysTick 1 ms, TIM2 (32 bit) 1 MHz free-running */
void g474_flash_init(void);           /* unlock state, error flags, page size from DBANK */
void g474_power_init(void);
void g474_ui_init(void);
void g474_ui_poll(void);              /* LED pattern + KEY debounce → pdk_local_start() */

#endif
