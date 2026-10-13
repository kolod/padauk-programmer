/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "power.h"

#include <string.h>

#include "hal_time.h"
#include "proto/proto_defs.h"

static int8_t cal[4];

void power_set_calibration(const int8_t c[4]) { memcpy(cal, c, sizeof cal); }
void power_get_calibration(int8_t c[4]) { memcpy(c, cal, sizeof cal); }

static uint32_t scale(uint32_t v, int32_t num, int32_t den)
{
    uint32_t r = (uint32_t)(((uint64_t)v * (uint32_t)num + (uint32_t)den / 2u) / (uint32_t)den);
    return r > 0xFFFFu ? 0xFFFFu : r;
}

void power_set_raw(pdk_rail_t rail, uint16_t mV)
{
    int8_t out = cal[rail == PDK_RAIL_VDD ? POWER_CAL_OUT_VDD : POWER_CAL_OUT_VPP];
    hal_power_set(rail, (uint16_t)scale(mV, 1000, 1000 + out));
}

uint16_t power_measure(pdk_rail_t rail)
{
    int8_t meas = cal[rail == PDK_RAIL_VDD ? POWER_CAL_MEAS_VDD : POWER_CAL_MEAS_VPP];
    return (uint16_t)scale(hal_power_measure(rail), 1000 + meas, 1000);
}

uint16_t power_tolerance_mV(uint16_t target_mV)
{
    uint32_t pct = (uint32_t)target_mV * POWER_TOL_PERCENT / 100u;
    return (uint16_t)(pct > POWER_TOL_MIN_MV ? pct : POWER_TOL_MIN_MV);
}

void power_off_all(void)
{
    hal_power_off_all();
}

uint16_t power_apply(pdk_rail_t rail, uint16_t mV, uint32_t min_pause_us, power_fault_t *fault)
{
    uint16_t tol = power_tolerance_mV(mV);
    power_set_raw(rail, mV);
    uint32_t start = hal_time_us();
    uint32_t last_sample = 0;
    int have_sample = 0;
    unsigned good = 0;
    uint16_t meas = 0;

    for (;;) {
        uint32_t now = hal_time_us();
        uint32_t elapsed = now - start;
        if (!have_sample || now - last_sample >= POWER_SAMPLE_GAP_US) {
            have_sample = 1;
            last_sample = now;
            meas = power_measure(rail);
            int diff = (int)meas - (int)mV;
            if (diff < 0)
                diff = -diff;
            good = (diff <= (int)tol) ? good + 1 : 0;
            if (good >= POWER_SAMPLES && elapsed >= min_pause_us)
                return PDK_OK;
        }
        if (elapsed > POWER_TIMEOUT_US)
            break;
    }

    hal_power_off_all();
    if (fault) {
        fault->rail = (uint8_t)rail;
        fault->set_mV = mV;
        fault->meas_mV = meas;
    }
    return rail == PDK_RAIL_VDD ? PDK_VDD_FAULT : PDK_VPP_FAULT;
}
