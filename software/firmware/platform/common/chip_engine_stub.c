/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chip engine stub for MCU platforms without the engine yet: the bit-level PADAUK algorithms come
 * with feature 002-chip-engine. Every primitive answers OP_UNSUPPORTED and leaves the chip in the
 * safe state. */
#include <stddef.h>

#include "chip_engine.h"
#include "hal_power.h"
#include "platform_common.h"
#include "power/power.h"
#include "proto/proto_defs.h"

void chip_engine_safe_state(void)
{
    hal_power_off_all();
    platform_ic_lines_hiz();
}

uint16_t chip_engine_probe(uint16_t *id12, uint8_t *family, chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

uint16_t chip_engine_read(const struct chip_info *chip, uint16_t addr, uint16_t count, chip_sink_fn sink, void *ctx,
                          chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

uint16_t chip_engine_erase(const struct chip_info *chip, chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

uint16_t chip_engine_write_block(const struct chip_info *chip, uint16_t addr, const uint16_t *words, uint8_t count,
                                 chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint32_t target_hz,
                               uint16_t mV, uint8_t *value, uint32_t *measured_hz, chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

uint16_t chip_engine_measure_freq(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint8_t start_value,
                                  uint16_t mV, uint32_t *hz, chip_poll_fn poll)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

/* RUN needs no PADAUK algorithm: VDD on (VPP stays 0), the lines stay Hi-Z for the UART bridge. */
uint16_t chip_engine_run_start(uint16_t vdd_mV)
{
    chip_engine_safe_state();
    uint16_t st = power_apply(PDK_RAIL_VDD, vdd_mV, POWER_PAUSE_VDD_CMD_US, NULL);
    if (st != PDK_OK)
        chip_engine_safe_state();
    return st;
}

void chip_engine_run_stop(void) { chip_engine_safe_state(); }
