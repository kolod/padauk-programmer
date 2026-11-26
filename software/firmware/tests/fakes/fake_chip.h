/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fake chip engine for unit tests: a chip in RAM with OTP/FLASH semantics (writes only clear bits,
 * FLASH can be erased), scripted calibration outcomes, failure injection and a hook that runs inside
 * every primitive just before it polls (to send commands, cancel or corrupt storage mid-operation). */
#ifndef PDK_FAKE_CHIP_H
#define PDK_FAKE_CHIP_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"

void fake_chip_reset(void);   /* no chip, no script (called by fake_reset) */

/* A blank chip of this type in the socket. */
void fake_chip_insert(const chip_info_t *chip);
void fake_chip_remove(void);
void fake_chip_set_id(uint16_t id12);   /* answers probe with another ID */
uint16_t *fake_chip_mem(void);          /* code_words words, direct access */

/* Calibration outcomes, consumed one per chip_engine_calibrate(); when the script is exhausted every
 * call succeeds with value 0x40. A call on a site that is not a measurement loop fails. */
typedef enum { FAKE_CAL_OK = 0, FAKE_CAL_FAIL, FAKE_CAL_WRONG_SITE } fake_cal_t;
void fake_chip_calib(fake_cal_t outcome, uint8_t value);

/* Writes to `addr` after the first one do not stick (a weak cell, FR-048). */
void fake_chip_weak(uint16_t addr);

/* What chip_engine_measure_freq() reports for an IHRC measurement loop (ILRC: hz / 128; 0 = no signal). */
void fake_chip_measure(uint32_t hz);

/* Called inside every primitive before each poll(). */
void fake_chip_hook(void (*fn)(void));

uint32_t fake_chip_writes(void);        /* chip_engine_write_block() calls */
uint32_t fake_chip_erases(void);
uint32_t fake_chip_safe_states(void);
uint32_t fake_chip_calibrations(void);

#endif
