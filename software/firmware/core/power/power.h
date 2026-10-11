/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Voltage settling policy (FR-040, contracts/protocol.md §6.1, contracts/chip-engine.md §2). */
#ifndef PDK_POWER_H
#define PDK_POWER_H

#include <stdint.h>

#include "hal_power.h"

/* Minimum pauses after a change, per stage (free-pdk timings). */
#define POWER_PAUSE_VPP_CMD_US 100u
#define POWER_PAUSE_VDD_CMD_US 500u
#define POWER_PAUSE_READ_US 1000u
#define POWER_PAUSE_HV_US 10000u

#define POWER_TIMEOUT_US 20000u      /* settle timeout */
#define POWER_SAMPLES 3u             /* consecutive in-tolerance samples */
#define POWER_SAMPLE_GAP_US 50u      /* spacing between samples */
#define POWER_TOL_PERCENT 2u         /* ±2 % ... */
#define POWER_TOL_MIN_MV 30u         /* ... or ±30 mV, whichever is larger */

typedef struct {
    uint8_t rail;       /* pdk_rail_t, 0xFF if none */
    uint16_t set_mV;
    uint16_t meas_mV;
} power_fault_t;

/* Sets the rail and waits until min_pause_us has elapsed AND the voltage is within tolerance for
 * POWER_SAMPLES samples. On timeout switches everything off and returns PDK_VDD_FAULT/PDK_VPP_FAULT. */
uint16_t power_apply(pdk_rail_t rail, uint16_t mV, uint32_t min_pause_us, power_fault_t *fault);

/* Tolerance used by power_apply() for a target voltage. */
uint16_t power_tolerance_mV(uint16_t target_mV);

/* Removes all voltages immediately. */
void power_off_all(void);

#endif
