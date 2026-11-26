/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic fake HAL for unit tests: RAM flash, virtual time, power model, captured transport. */
#ifndef PDK_FAKE_HAL_H
#define PDK_FAKE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_power.h"
#include "hal_ui.h"

/* Resets every fake to its initial state (erased flash, time 0, port open, supplies ideal). */
void fake_reset(void);
void fake_engine_reset(void);   /* provided by the linked engine fake (fake_chip / fake_icp) */

/* --- transport --- */
void fake_set_open(bool open);
void fake_rx_push(const uint8_t *data, size_t len);
size_t fake_tx_take(uint8_t *buf, size_t cap);   /* drains captured output */

/* --- time --- */
uint32_t fake_now_us(void);
void fake_advance_us(uint32_t us);

/* --- flash (a small geometry: 64 KB, 2 KB pages, slots 0x0800B000/0x0800D000, journal 0x0800F000) --- */
uint8_t *fake_flash_mem(uint32_t addr);   /* direct access for corruption tests */
uint32_t fake_flash_ops(void);            /* erase + program operations so far */
uint32_t fake_flash_erases(void);
/* After `n` more operations the next one is torn (half-done) and all further ops fail. */
void fake_flash_fail_after(uint32_t n);
bool fake_flash_failed(void);
/* Clears the failure state, keeping flash content (the next "power-on"). */
void fake_flash_power_restored(void);
/* Background erase model: hal_flash_erase_status() answers BUSY `n` times after each start. */
void fake_flash_bg_busy_polls(uint32_t n);
bool fake_flash_bg_violation(void);   /* a page was touched while erasing / overlapping erases */
uint32_t fake_flash_bg_started(void);
/* Program granularity (default 2 as on STM32F0; 8 models STM32G4 ECC flash). */
void fake_flash_write_unit(uint8_t unit);

/* --- power --- */
typedef enum { FAKE_POWER_OK = 0, FAKE_POWER_SLOW, FAKE_POWER_STUCK, FAKE_POWER_LOW } fake_power_mode_t;
void fake_power_mode(pdk_rail_t rail, fake_power_mode_t mode);
/* Scale errors in ppm (1000000 = none): output stage (true = set · out) and ADC (reading = true · meas). */
void fake_power_scale(pdk_rail_t rail, uint32_t out_ppm, uint32_t meas_ppm);
uint16_t fake_power_true_mV(pdk_rail_t rail);   /* what an external meter would show */
void fake_power_settle_us(uint32_t us);   /* time constant of an ideal supply */
uint16_t fake_power_set_value(pdk_rail_t rail);
uint32_t fake_power_off_calls(void);
void fake_power_hook(void (*fn)(pdk_rail_t rail, uint16_t mV));   /* every set point change (engine traces) */

/* --- entropy (default: xorshift noise; seed 0 = the reset seed) --- */
typedef enum {
    FAKE_ENTROPY_NOISE = 0,
    FAKE_ENTROPY_CONSTANT,   /* stuck source */
    FAKE_ENTROPY_PERIODIC,   /* short repeating pattern */
    FAKE_ENTROPY_BIASED,     /* mostly one value */
    FAKE_ENTROPY_FAIL,       /* hal_entropy_raw() reports failure */
} fake_entropy_mode_t;
void fake_entropy(fake_entropy_mode_t mode, uint32_t seed);
void fake_uid(const uint8_t uid[12]);

/* --- ui --- */
#define FAKE_UI_HISTORY 512u
pdk_ui_state_t fake_ui_last(void);
/* Every hal_ui_state() since fake_reset() / fake_ui_clear() (up to FAKE_UI_HISTORY). */
size_t fake_ui_history(const pdk_ui_state_t **states, const pdk_ui_detail_t **details);
void fake_ui_clear(void);

#endif
