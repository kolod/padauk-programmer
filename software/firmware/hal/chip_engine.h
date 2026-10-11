/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chip engine: bit-level PADAUK algorithms (feature 002-chip-engine; here a stub and a simulator model).
 *
 * Guarantees (contracts/chip-engine.md §3):
 *  1. ID before HV: primitives that apply write/erase voltages verify the chip ID first
 *     (mismatch → PDK_CHIP_ID_MISMATCH without high voltage).
 *  2. Voltages confirmed: no bit is sent to the chip until the last voltage change passed power_apply().
 *  3. Safe exit: every primitive leaves the chip in chip_engine_safe_state() on any exit path,
 *     unless the next step of the operation explicitly continues the session.
 *  4. Atomic steps < 100 ms: poll() is called at least every 100 ms.
 *  5. Cancel: when poll() returns false the primitive ends with PDK_CANCELLED after safe_state().
 */
#ifndef PDK_CHIP_ENGINE_H
#define PDK_CHIP_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

struct chip_info;

/* Called by primitives between atomic steps; returns false if the operation must be cancelled. */
typedef bool (*chip_poll_fn)(void);

/* Receives words read from the chip (stream). */
typedef void (*chip_sink_fn)(void *ctx, uint16_t addr, uint16_t word);

uint16_t chip_engine_probe(uint16_t *id12, uint8_t *family, chip_poll_fn poll);
uint16_t chip_engine_read(const struct chip_info *chip, uint16_t addr, uint16_t count,
                          chip_sink_fn sink, void *ctx, chip_poll_fn poll);
uint16_t chip_engine_erase(const struct chip_info *chip, chip_poll_fn poll);
uint16_t chip_engine_write_block(const struct chip_info *chip, uint16_t addr, const uint16_t *words,
                                 uint8_t count, chip_poll_fn poll);
uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind,
                               uint32_t target_hz, uint16_t mV, uint8_t *value, uint32_t *measured_hz,
                               chip_poll_fn poll);
uint16_t chip_engine_run_start(uint16_t vdd_mV);
void chip_engine_run_stop(void);
void chip_engine_safe_state(void);

#endif
