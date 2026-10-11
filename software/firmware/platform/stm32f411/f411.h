/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal interface of the STM32F411CEU6 platform: WeAct Studio MiniSTM32F4x1 ("Black Pill",
 * https://github.com/WeActStudio/WeActStudio.MiniSTM32F4x1) with a W25Q64 SPI NOR flash.
 *
 * Board (from the WeAct SDK examples):
 *   HSE 25 MHz, LSE 32.768 kHz; PC13 LED (active low); PA0 KEY (pull-up, pressed = 0);
 *   PA11/PA12 USB OTG FS (VBUS is not wired to PA9: VBUS sensing off);
 *   SPI1 PA5 SCK, PA6 MISO, PA7 MOSI, PA4 CS → W25Q64 (8 MB, 4 KB sectors, 256 B pages).
 *
 * The programming front end (VDD/VPP regulators, IC lines) is not defined yet: this platform has
 * no supplies, hal_power_limits() reports 0 V and every supply change ends in VDD/VPP_FAULT.
 */
#ifndef PDK_F411_H
#define PDK_F411_H

#include <stdbool.h>
#include <stdint.h>

#include "platform_common.h"
#include "stm32f4xx.h"

#define F411_HCLK_HZ 96000000u   /* PLL: 25 MHz / 25 × 192 / 2; USB 48 MHz = VCO / 4 */
#define F411_APB1_HZ 48000000u
#define F411_APB2_HZ 96000000u

void f411_clock_init(void);
void f411_time_init(void);    /* SysTick 1 ms, TIM2 (32 bit) 1 MHz free-running */
void f411_adc_init(void);     /* ADC1 for entropy (temperature sensor, VREFINT) */
uint16_t f411_adc_convert(uint32_t channel);
void f411_ui_init(void);
void f411_ui_poll(void);      /* LED pattern + KEY debounce → pdk_local_start() */

/* W25Q64 on SPI1. Returns false if the JEDEC ID is not a Winbond W25Q (no flash fitted). */
bool w25q_init(void);
bool w25q_present(void);
void w25q_read(uint32_t addr, uint8_t *buf, uint32_t len);
bool w25q_erase_sector(uint32_t addr);                              /* 4 KB, aligned; blocking */
bool w25q_erase_start(uint32_t addr);                               /* background erase */
bool w25q_erase_done(void);                                         /* finished or timed out */
bool w25q_erase_timed_out(void);                                    /* of the last erase; clears */
bool w25q_program(uint32_t addr, const uint8_t *data, uint32_t len);  /* splits at 256 B pages */

#endif
