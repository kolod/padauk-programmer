/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SELF_TEST (contracts/protocol.md §4, research R15): PROBE at low voltages first — a chip that answers
 * gets nothing higher (SELFTEST_CHIP_PRESENT); then every distinct VDD level of the chip table in
 * ascending order and 0 V (VPP = 0), then the same for VPP (VDD = 0), each by the FR-040 rule. The first
 * level not reached ends it with SELFTEST_FAILED. Runs at start-up (source = local) and from the PC. */
#include "chip_engine.h"
#include "chips/chip_table.h"
#include "hal_time.h"
#include "op_manager.h"
#include "power/power.h"
#include "proto/proto_defs.h"

static uint8_t state = PDK_SELFTEST_NOT_RUN;

uint8_t op_selftest_state(void) { return state; }

/* Distinct non-zero values of one rail over the chip table, ascending, within the hardware limit. */
static uint8_t rail_levels(pdk_rail_t rail, uint16_t limit, uint16_t *out, uint8_t max)
{
    uint8_t n = 0;
    for (size_t i = 0; i < chip_count(); i++) {
        const chip_info_t *c = chip_at(i);
        uint16_t v[6];
        if (rail == PDK_RAIL_VDD) {
            v[0] = c->vdd_cmd_read, v[1] = c->vdd_read, v[2] = c->vdd_cmd_write;
            v[3] = c->vdd_write, v[4] = c->vdd_cmd_erase, v[5] = c->vdd_erase;
        } else {
            v[0] = c->vpp_cmd_read, v[1] = c->vpp_read, v[2] = c->vpp_cmd_write;
            v[3] = c->vpp_write, v[4] = c->vpp_cmd_erase, v[5] = c->vpp_erase;
        }
        for (unsigned k = 0; k < 6; k++) {
            if (!v[k] || v[k] > limit)
                continue;
            unsigned j = 0;
            while (j < n && out[j] < v[k])
                j++;
            if (j < n && out[j] == v[k])
                continue;
            if (n == max)
                return n;
            for (unsigned m = n; m > j; m--)
                out[m] = out[m - 1];
            out[j] = v[k];
            n++;
        }
    }
    return n;
}

/* One level; false = not reached (supplies already off, RESULT filled). */
static bool level(op_result_t *r, pdk_rail_t rail, uint16_t mV)
{
    uint32_t t0 = hal_time_us();
    power_fault_t f;
    uint16_t st = power_apply(rail, mV, rail == PDK_RAIL_VDD ? POWER_PAUSE_VDD_CMD_US : POWER_PAUSE_VPP_CMD_US, &f);
    uint32_t dt = hal_time_us() - t0;
    if (r->level_count < OP_MAX_LEVELS) {
        op_level_t *l = &r->levels[r->level_count++];
        l->rail = (uint8_t)rail;
        l->set_mV = mV;
        l->meas_mV = st == PDK_OK ? power_measure(rail) : f.meas_mV;
        l->settle_us = (uint16_t)(dt > 0xFFFFu ? 0xFFFFu : dt);
        l->ok = st == PDK_OK;
    }
    if (st != PDK_OK) {
        r->v_rail = f.rail;
        r->v_set_mV = f.set_mV;
        r->v_meas_mV = f.meas_mV;
    }
    return st == PDK_OK;
}

uint16_t op_selftest_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_SELF_TEST};
    op_plan(stages, sizeof stages);
    op_result_t *r = op_current();
    uint16_t limit[2];
    hal_power_limits(&limit[PDK_RAIL_VDD], &limit[PDK_RAIL_VPP]);
    if (!limit[PDK_RAIL_VDD] || !limit[PDK_RAIL_VPP])
        return PDK_OP_UNSUPPORTED;   /* no supply front end */

    state = PDK_SELFTEST_RUNNING;
    op_stage(PDK_STAGE_ID);
    uint16_t id = 0;
    uint8_t family = 0;
    uint16_t st = chip_engine_probe(&id, &family, op_poll);
    chip_engine_safe_state();
    if (st == PDK_OK) {   /* a chip in the socket: no higher voltage */
        r->chip_id_found = id;
        r->family_found = family;
        state = PDK_SELFTEST_CHIP_PRESENT_STATE;
        return PDK_SELFTEST_CHIP_PRESENT;
    }
    if (st == PDK_OP_UNSUPPORTED) {   /* the engine cannot tell whether a chip is there: no high voltage */
        state = PDK_SELFTEST_NOT_RUN;
        return PDK_OP_UNSUPPORTED;
    }
    if (st != PDK_CHIP_NOT_FOUND) {   /* the probe voltages themselves did not settle */
        state = PDK_SELFTEST_FAILED_STATE;
        if (st == PDK_VDD_FAULT || st == PDK_VPP_FAULT) {   /* reported as the level that was not reached */
            const power_fault_t *f = power_last_fault();
            r->v_rail = f->rail;
            r->v_set_mV = f->set_mV;
            r->v_meas_mV = f->meas_mV;
            r->levels[0] = (op_level_t){f->rail, f->set_mV, f->meas_mV, (uint16_t)POWER_TIMEOUT_US, 0};
            r->level_count = 1;
            return PDK_SELFTEST_FAILED;
        }
        return st;
    }

    op_stage(PDK_STAGE_SELF_TEST);
    static const pdk_rail_t rails[2] = {PDK_RAIL_VDD, PDK_RAIL_VPP};
    uint16_t v[OP_MAX_LEVELS];
    for (unsigned k = 0; k < 2; k++) {
        pdk_rail_t rail = rails[k];
        uint8_t n = rail_levels(rail, limit[rail], v, (uint8_t)(OP_MAX_LEVELS / 2u - 1u));
        for (uint8_t i = 0; i <= n; i++) {   /* the levels, then 0 V */
            op_percent((uint8_t)((k * (n + 1u) + i) * 50u / (n + 1u)));
            if (!level(r, rail, i < n ? v[i] : 0)) {
                chip_engine_safe_state();
                state = PDK_SELFTEST_FAILED_STATE;
                return PDK_SELFTEST_FAILED;
            }
            if (!op_poll()) {
                chip_engine_safe_state();
                state = PDK_SELFTEST_NOT_RUN;
                return PDK_CANCELLED;
            }
        }
    }
    chip_engine_safe_state();
    state = PDK_SELFTEST_PASSED;
    return PDK_OK;
}
