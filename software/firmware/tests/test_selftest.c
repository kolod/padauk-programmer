/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SELF_TEST on the fake supplies and chip engine (T093): the levels of the chip table in ascending order
 * and 0 V per rail, probe first, a chip in the socket stops it, the first level not reached fails it,
 * start-up run with source = local, STATUS.selftest_state. */
#include "pdk_test.h"

#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_ops.h"

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    pdk_core_poll();   /* the start-up SELF_TEST */
    tops_seq = 1;
}

/* Distinct non-zero values of a rail in the chip table, ascending (an independent count). */
static int expected_levels(pdk_rail_t rail, uint16_t *out)
{
    int n = 0;
    for (size_t i = 0; i < chip_count(); i++) {
        const chip_info_t *c = chip_at(i);
        uint16_t v[6] = {c->vdd_cmd_read, c->vdd_read, c->vdd_cmd_write, c->vdd_write, c->vdd_cmd_erase, c->vdd_erase};
        if (rail == PDK_RAIL_VPP) {
            uint16_t w[6] = {c->vpp_cmd_read, c->vpp_read, c->vpp_cmd_write, c->vpp_write, c->vpp_cmd_erase,
                             c->vpp_erase};
            memcpy(v, w, sizeof v);
        }
        for (int k = 0; k < 6; k++) {
            bool seen = !v[k];
            for (int j = 0; j < n && !seen; j++)
                seen = out[j] == v[k];
            if (!seen)
                out[n++] = v[k];
        }
    }
    for (int i = 1; i < n; i++)   /* insertion sort */
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--) {
            uint16_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

static uint8_t status_selftest(uint8_t *flags)
{
    tp_frame_t r;
    tp_hello(tops_seq++);
    tp_call(tops_seq++, PDK_CMD_GET_STATUS, NULL, 0, &r);
    *flags = r.payload[19];
    return r.payload[23];
}

TEST(start_up_run_passes_without_a_chip)
{
    const op_result_t *r = op_manager_result(0);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->op_type, PDK_OP_SELF_TEST);
    ASSERT_EQ(r->source, PDK_SOURCE_LOCAL);
    ASSERT_EQ(r->status, PDK_OK);
    uint8_t flags;
    ASSERT_EQ(status_selftest(&flags), PDK_SELFTEST_PASSED);
    ASSERT_EQ(flags & PDK_FLAG_LOCAL_START_BLOCKED, 0);
}

TEST(levels_in_ascending_order_then_zero_other_rail_off)
{
    const op_result_t *r = tops_run(PDK_OP_SELF_TEST, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_SELF_TEST);
    uint16_t vdd[16], vpp[16];
    int nd = expected_levels(PDK_RAIL_VDD, vdd), np = expected_levels(PDK_RAIL_VPP, vpp);
    ASSERT_EQ(r->level_count, nd + 1 + np + 1);
    for (int i = 0; i <= nd; i++) {
        ASSERT_EQ(r->levels[i].rail, PDK_RAIL_VDD);
        ASSERT_EQ(r->levels[i].set_mV, i < nd ? vdd[i] : 0);
        ASSERT_EQ(r->levels[i].ok, 1);
    }
    for (int i = 0; i <= np; i++) {
        const op_level_t *l = &r->levels[nd + 1 + i];
        ASSERT_EQ(l->rail, PDK_RAIL_VPP);
        ASSERT_EQ(l->set_mV, i < np ? vpp[i] : 0);
        ASSERT_TRUE(l->settle_us > 0);
    }
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(a_chip_in_the_socket_gets_no_high_voltage)
{
    fake_chip_insert(tops_chip("PFS154"));
    const op_result_t *r = tops_run(PDK_OP_SELF_TEST, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_SELFTEST_CHIP_PRESENT);
    ASSERT_EQ(r->stage, PDK_STAGE_ID);
    ASSERT_EQ(r->level_count, 0);
    ASSERT_EQ(r->chip_id_found, 0xAA1);
    uint8_t flags;
    ASSERT_EQ(status_selftest(&flags), PDK_SELFTEST_CHIP_PRESENT_STATE);
    ASSERT_TRUE(flags & PDK_FLAG_LOCAL_START_BLOCKED);
}

TEST(the_first_level_not_reached_fails_it)
{
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_STUCK);
    uint32_t safe = fake_chip_safe_states();
    const op_result_t *r = tops_run(PDK_OP_SELF_TEST, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_SELFTEST_FAILED);
    uint16_t vdd[16];
    int nd = expected_levels(PDK_RAIL_VDD, vdd);
    ASSERT_EQ(r->level_count, nd + 1 + 1);   /* every VDD level and 0 V, then the first VPP level */
    const op_level_t *bad = &r->levels[r->level_count - 1];
    ASSERT_EQ(bad->rail, PDK_RAIL_VPP);
    ASSERT_EQ(bad->ok, 0);
    ASSERT_EQ(r->v_rail, PDK_RAIL_VPP);
    ASSERT_EQ(r->v_set_mV, bad->set_mV);
    ASSERT_TRUE(fake_chip_safe_states() > safe);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    uint8_t flags;
    ASSERT_EQ(status_selftest(&flags), PDK_SELFTEST_FAILED_STATE);
    ASSERT_TRUE(flags & PDK_FLAG_LOCAL_START_BLOCKED);
}
