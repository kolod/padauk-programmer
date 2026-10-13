/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supply_cal.h"

#include <string.h>

#include "hal_time.h"
#include "power.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"

#define CAL_POINT_PERCENT 90u          /* of the rail limit: below op-amp saturation */
#define CHECK_VDD_MV 6000u
#define CHECK_VPP_MV 12000u
#define CHECK_MAX_PERCENT 95u          /* check voltages are clamped to this share of the limit */
#define SETTLE_TIMEOUT_US 50000u
#define STABLE_SAMPLES 8u
#define AVERAGE_SAMPLES 16u
#define IDLE_TIMEOUT_MS (5u * 60u * 1000u)   /* voltages are not left on forever */
#define TRIM_MAX 127

typedef enum { CAL_IDLE = 0, CAL_RAISED, CAL_VERIFY } cal_state_t;

static struct {
    cal_state_t state;
    int8_t saved[4];          /* calibration in effect before CAL_START */
    int8_t candidate[4];
    uint16_t point[2];        /* set points of CAL_START, by rail */
    uint16_t raw[2];          /* uncalibrated measurements at those points */
    uint32_t last_ms;
} c;

void supply_cal_init(void)
{
    memset(&c, 0, sizeof c);
    const journal_state_t *j = journal_get();
    int8_t cal[4] = {0, 0, 0, 0};
    if (j->flags & JOURNAL_FLAG_SUPPLY_CAL)
        memcpy(cal, j->supply_cal, sizeof cal);
    power_set_calibration(cal);
}

bool supply_cal_active(void) { return c.state != CAL_IDLE; }

void supply_cal_abort(void)
{
    if (c.state == CAL_IDLE)
        return;
    power_off_all();
    power_set_calibration(c.saved);
    c.state = CAL_IDLE;
}

void supply_cal_poll(void)
{
    if (c.state != CAL_IDLE && hal_time_ms() - c.last_ms > IDLE_TIMEOUT_MS)
        supply_cal_abort();
}

static bool supplies_present(uint16_t *vdd_max, uint16_t *vpp_max)
{
    hal_power_limits(vdd_max, vpp_max);
    return *vdd_max && *vpp_max;
}

static uint16_t average(pdk_rail_t rail)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < AVERAGE_SAMPLES; i++)
        sum += power_measure(rail);
    return (uint16_t)((sum + AVERAGE_SAMPLES / 2u) / AVERAGE_SAMPLES);
}

/* Waits until both rails stop moving (calibration may be far off, so no tolerance check). */
static void wait_stable(void)
{
    uint16_t prev[2] = {0, 0};
    unsigned stable = 0;
    uint32_t t0 = hal_time_us();
    while (stable < STABLE_SAMPLES && hal_time_us() - t0 < SETTLE_TIMEOUT_US) {
        uint16_t m[2] = {power_measure(PDK_RAIL_VDD), power_measure(PDK_RAIL_VPP)};
        bool same = true;
        for (int r = 0; r < 2; r++) {
            int d = (int)m[r] - (int)prev[r];
            same = same && d >= -10 && d <= 10;
            prev[r] = m[r];
        }
        stable = same ? stable + 1 : 0;
        hal_delay_us(POWER_SAMPLE_GAP_US);
    }
}

static void put_fault(uint8_t *rsp, uint16_t *rsp_len, pdk_rail_t rail, uint16_t set, uint16_t meas)
{
    rsp[0] = (uint8_t)rail;
    put16(rsp + 1, set);
    put16(rsp + 3, meas);
    *rsp_len = 5;
}

uint16_t supply_cal_cmd_start(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    uint16_t vdd_max, vpp_max;
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    if (!supplies_present(&vdd_max, &vpp_max))
        return PDK_OP_UNSUPPORTED;
    if (c.state == CAL_IDLE)
        power_get_calibration(c.saved);
    static const int8_t none[4] = {0, 0, 0, 0};
    power_off_all();
    power_set_calibration(none);   /* the coefficients are derived from the raw scale */

    c.point[PDK_RAIL_VDD] = (uint16_t)((uint32_t)vdd_max * CAL_POINT_PERCENT / 100u);
    c.point[PDK_RAIL_VPP] = (uint16_t)((uint32_t)vpp_max * CAL_POINT_PERCENT / 100u);
    power_set_raw(PDK_RAIL_VDD, c.point[PDK_RAIL_VDD]);
    power_set_raw(PDK_RAIL_VPP, c.point[PDK_RAIL_VPP]);
    wait_stable();
    for (int r = 0; r < 2; r++) {
        c.raw[r] = average((pdk_rail_t)r);
        if (c.raw[r] < c.point[r] / 2u) {   /* rail dead or shorted: not a calibration matter */
            put_fault(rsp, rsp_len, (pdk_rail_t)r, c.point[r], c.raw[r]);
            c.state = CAL_RAISED;
            supply_cal_abort();
            return r == PDK_RAIL_VDD ? PDK_VDD_FAULT : PDK_VPP_FAULT;
        }
    }
    put16(rsp + 0, c.point[PDK_RAIL_VDD]);
    put16(rsp + 2, c.point[PDK_RAIL_VPP]);
    put16(rsp + 4, c.raw[PDK_RAIL_VDD]);
    put16(rsp + 6, c.raw[PDK_RAIL_VPP]);
    *rsp_len = 8;
    c.state = CAL_RAISED;
    c.last_ms = hal_time_ms();
    return PDK_OK;
}

/* (true / reference − 1) in per mille, rounded; false if outside ±TRIM_MAX. */
static bool trim(uint16_t true_mV, uint16_t ref_mV, int8_t *out)
{
    if (!ref_mV)
        return false;
    int32_t pm = ((int32_t)true_mV * 1000 - (int32_t)ref_mV * 1000);
    pm = (pm >= 0 ? pm + ref_mV / 2 : pm - (int32_t)ref_mV / 2) / (int32_t)ref_mV;
    if (pm < -TRIM_MAX || pm > TRIM_MAX)
        return false;
    *out = (int8_t)pm;
    return true;
}

uint16_t supply_cal_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (c.state != CAL_RAISED)
        return PDK_NOT_ALLOWED_NOW;
    if (len != 4)
        return PDK_BAD_PAYLOAD;
    c.last_ms = hal_time_ms();
    uint16_t true_mV[2] = {le16(req), le16(req + 2)};
    int8_t cal[4];
    for (int r = 0; r < 2; r++) {
        if (!trim(true_mV[r], c.raw[r], &cal[r == 0 ? POWER_CAL_MEAS_VDD : POWER_CAL_MEAS_VPP]) ||
            !trim(true_mV[r], c.point[r], &cal[r == 0 ? POWER_CAL_OUT_VDD : POWER_CAL_OUT_VPP])) {
            rsp[0] = (uint8_t)r;   /* which value is implausible; the rails stay up for a retry */
            *rsp_len = 1;
            return PDK_BAD_PAYLOAD;
        }
    }

    /* exact check voltages with the new coefficients (lower only on boards with a low limit) */
    uint16_t limit[2];
    hal_power_limits(&limit[0], &limit[1]);
    uint16_t target[2] = {CHECK_VDD_MV, CHECK_VPP_MV};
    for (int r = 0; r < 2; r++) {
        uint16_t max = (uint16_t)((uint32_t)limit[r] * CHECK_MAX_PERCENT / 100u);
        if (target[r] > max)
            target[r] = max;
    }
    memcpy(c.candidate, cal, sizeof cal);
    power_set_calibration(cal);
    for (int r = 0; r < 2; r++) {
        power_fault_t f;
        uint16_t st = power_apply((pdk_rail_t)r, target[r], POWER_PAUSE_HV_US, &f);
        if (st != PDK_OK) {
            put_fault(rsp, rsp_len, (pdk_rail_t)f.rail, f.set_mV, f.meas_mV);
            supply_cal_abort();
            return st;
        }
    }
    put16(rsp + 0, target[0]);
    put16(rsp + 2, target[1]);
    put16(rsp + 4, average(PDK_RAIL_VDD));
    put16(rsp + 6, average(PDK_RAIL_VPP));
    memcpy(rsp + 8, cal, 4);
    *rsp_len = 12;
    c.state = CAL_VERIFY;
    c.last_ms = hal_time_ms();
    return PDK_OK;
}

uint16_t supply_cal_cmd_confirm(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 1 || req[0] > 1)
        return PDK_BAD_PAYLOAD;
    if (c.state == CAL_IDLE)
        return PDK_NOT_ALLOWED_NOW;
    if (!req[0] || c.state != CAL_VERIFY) {   /* rejected (or nothing to accept yet) */
        supply_cal_abort();
        return req[0] ? PDK_NOT_ALLOWED_NOW : PDK_OK;
    }
    power_off_all();
    c.state = CAL_IDLE;
    if (!package_store_save_supply_cal(c.candidate, true)) {
        power_set_calibration(c.saved);
        return PDK_STORE_WRITE_FAILED;
    }
    power_set_calibration(c.candidate);
    return PDK_OK;
}

uint16_t supply_cal_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    const journal_state_t *j = journal_get();
    rsp[0] = (j->flags & JOURNAL_FLAG_SUPPLY_CAL) ? 1 : 0;
    memcpy(rsp + 1, j->supply_cal, 4);
    if (!rsp[0])
        memset(rsp + 1, 0, 4);
    rsp[5] = (uint8_t)c.state;
    *rsp_len = 6;
    return PDK_OK;
}

uint16_t supply_cal_cmd_clear(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (c.state != CAL_IDLE)
        return PDK_NOT_ALLOWED_NOW;
    static const int8_t none[4] = {0, 0, 0, 0};
    if (!package_store_save_supply_cal(none, false))
        return PDK_STORE_WRITE_FAILED;
    power_set_calibration(none);
    return PDK_OK;
}
