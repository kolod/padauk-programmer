/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running the IC (T035): chip_engine_run_start() leaves any open session, applies VDD only (VPP stays 0) through
 * power_apply(), leaves the ICP lines released, refuses a VDD above hal_icp_vdd_max_mV() without any voltage;
 * chip_engine_run_stop() ends in the safe state. */
#include "pdk_test.h"

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "engine_internal.h"
#include "fake_hal.h"
#include "fake_icp.h"
#include "fake_icp_ic.h"
#include "proto/proto_defs.h"

static const chip_info_t *pfs154;

void pdk_test_setup(void)
{
    fake_reset();
    eng_ses.pause_us = 0;
    eng_ses.state = SES_IDLE;
    fake_icp_record(false);
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), "PFS154"))
            pfs154 = chip_at(i);
}

static void sink(void *ctx, uint16_t addr, uint16_t word) { (void)ctx, (void)addr, (void)word; }

static bool lines_released(void)
{
    static const icp_line_t lines[] = {ICP_CLK, ICP_DAT, ICP_PA4, ICP_PA0, ICP_PA7};
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        if (fake_icp_driven(lines[i], NULL))
            return false;
    return true;
}

TEST(run_applies_vdd_only_and_releases_the_lines)
{
    fake_ic_insert(pfs154);
    ASSERT_EQ(chip_engine_run_start(3300), PDK_OK);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 3300);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_EQ(fake_ic_max_vpp(), 0);
    ASSERT_TRUE(lines_released());
    ASSERT_EQ(eng_ses.mode, MODE_RUN);
    ASSERT_EQ(fake_ic_enters(), 0u);   /* never in programming mode */
}

TEST(run_leaves_an_open_session_first)
{
    fake_ic_insert(pfs154);
    uint16_t blk[4] = {0x0070, 0x0071, 0x0072, 0x0073};
    ASSERT_EQ(chip_engine_write_block(pfs154, 0, blk, 4, NULL), PDK_OK);
    ASSERT_EQ(eng_ses.state, SES_HV);
    fake_icp_record(true);
    ASSERT_EQ(chip_engine_run_start(5000), PDK_OK);
    /* the write session is left (both supplies off, lines idle) before VDD comes back alone */
    const fake_ev_t *ev;
    size_t n = fake_icp_events(&ev);
    size_t idle = n, vdd_on = n;
    for (size_t i = 0; i < n; i++) {
        if (ev[i].kind == FEV_IDLE && idle == n)
            idle = i;
        if (ev[i].kind == FEV_VDD && ev[i].mV == 5000)
            vdd_on = i;
        ASSERT_TRUE(!(ev[i].kind == FEV_VPP && ev[i].mV != 0));
    }
    ASSERT_TRUE(idle < vdd_on && vdd_on < n);
    /* the post-write pause was respected */
    ASSERT_TRUE(ev[vdd_on].t_us - ev[idle].t_us >= ENG_PAUSE_LEAVE_US + ENG_PAUSE_LEAVE_HV_US);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_TRUE(lines_released());
}

TEST(run_above_the_pin_limit_is_refused_without_voltage)
{
    fake_ic_insert(pfs154);
    fake_icp_vdd_limit(5000);
    ASSERT_EQ(chip_engine_run_start(5500), PDK_OP_UNSUPPORTED);
    ASSERT_EQ(fake_ic_max_vdd(), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(eng_ses.state, SES_IDLE);
    ASSERT_EQ(chip_engine_run_start(5000), PDK_OK);   /* at the limit: allowed */
    chip_engine_run_stop();
}

TEST(run_with_a_supply_fault_ends_safe)
{
    fake_power_mode(PDK_RAIL_VDD, FAKE_POWER_STUCK);
    ASSERT_TRUE(chip_engine_run_start(3300) != PDK_OK);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(eng_ses.state, SES_IDLE);
}

TEST(run_stop_is_the_safe_state)
{
    fake_ic_insert(pfs154);
    ASSERT_EQ(chip_engine_run_start(3300), PDK_OK);
    chip_engine_run_stop();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_TRUE(lines_released());
    ASSERT_EQ(eng_ses.state, SES_IDLE);
    chip_engine_run_stop();   /* twice: harmless */
    ASSERT_EQ(eng_ses.state, SES_IDLE);
}

TEST(a_read_after_run_waits_the_leave_pause)
{
    fake_ic_insert(pfs154);
    ASSERT_EQ(chip_engine_run_start(3300), PDK_OK);
    chip_engine_run_stop();
    uint32_t t0 = fake_now_us();
    ASSERT_EQ(chip_engine_read(pfs154, 0, 2, sink, NULL, NULL), PDK_OK);
    ASSERT_TRUE(fake_now_us() - t0 >= ENG_PAUSE_LEAVE_US);
    chip_engine_safe_state();
}
