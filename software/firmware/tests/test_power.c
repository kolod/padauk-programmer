/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_test.h"

#include "fake_hal.h"
#include "power/power.h"
#include "proto/proto_defs.h"

void pdk_test_setup(void) { fake_reset(); }

TEST(tolerance_is_2_percent_or_30_mV)
{
    ASSERT_EQ(power_tolerance_mV(10500), 210);
    ASSERT_EQ(power_tolerance_mV(2000), 40);
    ASSERT_EQ(power_tolerance_mV(1000), 30);
    ASSERT_EQ(power_tolerance_mV(0), 30);
}

TEST(waits_at_least_min_pause_even_if_settled)
{
    fake_power_settle_us(0);
    uint32_t t0 = fake_now_us();
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 2500, POWER_PAUSE_HV_US, &f), PDK_OK);
    ASSERT_TRUE(fake_now_us() - t0 >= POWER_PAUSE_HV_US);
}

TEST(waits_until_voltage_settles)
{
    fake_power_settle_us(3000);
    uint32_t t0 = fake_now_us();
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VPP, 8000, POWER_PAUSE_VPP_CMD_US, &f), PDK_OK);
    /* settled only near the end of the ramp, after the 100 us pause */
    ASSERT_TRUE(fake_now_us() - t0 >= 2500);
    ASSERT_TRUE(fake_now_us() - t0 < POWER_TIMEOUT_US);
}

TEST(stuck_supply_times_out_with_fault_and_power_off)
{
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_STUCK);
    uint32_t offs = fake_power_off_calls();
    power_fault_t f = {0};
    uint32_t t0 = fake_now_us();
    ASSERT_EQ(power_apply(PDK_RAIL_VPP, 10500, POWER_PAUSE_HV_US, &f), PDK_VPP_FAULT);
    uint32_t dt = fake_now_us() - t0;
    ASSERT_TRUE(dt > POWER_TIMEOUT_US && dt < POWER_TIMEOUT_US + 1000);
    ASSERT_EQ(f.rail, PDK_RAIL_VPP);
    ASSERT_EQ(f.set_mV, 10500);
    ASSERT_EQ(f.meas_mV, 0);
    ASSERT_EQ(fake_power_off_calls(), offs + 1);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(low_supply_is_vdd_fault)
{
    fake_power_mode(PDK_RAIL_VDD, FAKE_POWER_LOW);
    power_fault_t f = {0};
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5800, POWER_PAUSE_HV_US, &f), PDK_VDD_FAULT);
    ASSERT_EQ(f.rail, PDK_RAIL_VDD);
    ASSERT_EQ(f.meas_mV, 5220);
}

TEST(slow_supply_beyond_timeout_fails)
{
    fake_power_mode(PDK_RAIL_VDD, FAKE_POWER_SLOW);   /* 50 ms ramp */
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5000, POWER_PAUSE_VDD_CMD_US, &f), PDK_VDD_FAULT);
}

TEST(zero_volts_uses_same_rule)
{
    fake_power_settle_us(1000);
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5000, 0, &f), PDK_OK);
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 0, 0, &f), PDK_OK);
    fake_power_mode(PDK_RAIL_VDD, FAKE_POWER_STUCK);
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5000, 0, &f), PDK_VDD_FAULT);
}
