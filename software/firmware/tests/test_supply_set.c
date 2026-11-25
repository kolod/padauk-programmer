/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Arbitrary VDD/VPP for checking the supply front end (SUPPLY_SET / SUPPLY_GET). */
#include "pdk_test.h"

#include <stdlib.h>

#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_proto.h"

static tp_frame_t r;
static uint16_t seq;

static uint16_t call(uint16_t code, const void *p, uint16_t len)
{
    tp_call(seq++, code, p, len, &r);
    return r.status;
}

static uint16_t supply_flags(uint16_t vdd, uint16_t vpp, uint8_t flags)
{
    uint8_t p[5];
    put16(p, vdd);
    put16(p + 2, vpp);
    p[4] = flags;
    return call(PDK_CMD_SUPPLY_SET, p, 5);
}

static uint16_t supply(uint16_t vdd, uint16_t vpp)
{
    uint8_t p[4];
    put16(p, vdd);
    put16(p + 2, vpp);
    return call(PDK_CMD_SUPPLY_SET, p, 4);
}

static int pct_off(uint16_t value, uint16_t target)   /* deviation in 0.01 % */
{
    return (int)(((int32_t)value - target) * 10000 / target);
}

/* the G474-like board of test_supply_cal.c, calibrated the way pdkprog does it */
static void calibrate(void)
{
    uint8_t p[4];
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    put16(p, fake_power_true_mV(PDK_RAIL_VDD));
    put16(p + 2, fake_power_true_mV(PDK_RAIL_VPP));
    ASSERT_EQ(call(PDK_CMD_CAL_SET, p, 4), PDK_OK);
    uint8_t accept = 1;
    ASSERT_EQ(call(PDK_CMD_CAL_CONFIRM, &accept, 1), PDK_OK);
}

void pdk_test_setup(void)
{
    fake_reset();
    fake_power_scale(PDK_RAIL_VDD, 945000, 1012000);
    fake_power_scale(PDK_RAIL_VPP, 948000, 988000);
    pdk_core_init();
    seq = 1;
    tp_hello(seq++);
}

TEST(info_reports_the_capability)
{
    ASSERT_EQ(call(PDK_CMD_GET_INFO, NULL, 0), PDK_OK);
    ASSERT_TRUE(le32(r.payload + 10) & PDK_CAP_SUPPLY_SET);
}

TEST(calibrated_rails_reach_the_requested_voltages)
{
    calibrate();
    static const uint16_t levels[][2] = {{2500, 0}, {3300, 5000}, {5000, 8000}, {5800, 12500}, {1800, 0}};
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        ASSERT_EQ(supply(levels[i][0], levels[i][1]), PDK_OK);
        ASSERT_EQ(r.len, 16);
        ASSERT_EQ(le16(r.payload + 0), levels[i][0]);
        ASSERT_EQ(le16(r.payload + 2), levels[i][1]);
        ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VDD), levels[i][0])) <= 50);   /* meter */
        ASSERT_TRUE(abs(pct_off(le16(r.payload + 4), levels[i][0])) <= 50);                /* reading */
        if (levels[i][1])
            ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VPP), levels[i][1])) <= 50);
        else
            ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    }
}

TEST(get_reports_set_points_and_readings)
{
    calibrate();
    ASSERT_EQ(supply(3300, 9000), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_SUPPLY_GET, NULL, 0), PDK_OK);
    ASSERT_EQ(r.len, 16);
    ASSERT_EQ(le16(r.payload + 0), 3300);
    ASSERT_EQ(le16(r.payload + 2), 9000);
    ASSERT_TRUE(abs(pct_off(le16(r.payload + 6), 9000)) <= 50);
    for (int rail = 0; rail < 2; rail++) {   /* min <= average <= max */
        uint16_t avg = le16(r.payload + 4 + 2 * rail), lo = le16(r.payload + 8 + 4 * rail),
                 hi = le16(r.payload + 10 + 4 * rail);
        ASSERT_TRUE(lo <= avg && avg <= hi);
        ASSERT_TRUE(lo > 0);
    }
}

TEST(no_check_keeps_an_out_of_tolerance_rail_up)
{
    ASSERT_EQ(supply_flags(5000, 0, 0x01), PDK_OK);   /* uncalibrated: the output stage is 5.5 % low */
    ASSERT_EQ(le16(r.payload + 0), 5000);
    ASSERT_TRUE(fake_power_set_value(PDK_RAIL_VDD) > 0);
    ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VDD), 4725)) <= 50);
    ASSERT_EQ(supply_flags(0, 0, 0), PDK_OK);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
}

TEST(unknown_flags_are_refused)
{
    ASSERT_EQ(supply_flags(3300, 0, 0x02), PDK_BAD_PAYLOAD);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
}

TEST(uncalibrated_scale_error_is_a_fault_and_everything_goes_off)
{
    ASSERT_EQ(supply(5000, 0), PDK_VDD_FAULT);   /* the output stage is 5.5 % low */
    ASSERT_EQ(r.len, 5);
    ASSERT_EQ(r.payload[0], PDK_RAIL_VDD);
    ASSERT_EQ(le16(r.payload + 1), 5000);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(call(PDK_CMD_SUPPLY_GET, NULL, 0), PDK_OK);
    ASSERT_EQ(le16(r.payload + 0), 0);
}

TEST(dead_rail_is_a_fault)
{
    calibrate();
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_STUCK);
    ASSERT_EQ(supply(5000, 12000), PDK_VPP_FAULT);
    ASSERT_EQ(r.payload[0], PDK_RAIL_VPP);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(above_the_limit_is_refused_without_touching_the_rails)
{
    calibrate();
    ASSERT_EQ(supply(3300, 13271), PDK_BAD_PAYLOAD);   /* fake limits 6260 / 13270 mV */
    ASSERT_EQ(r.payload[0], PDK_RAIL_VPP);
    ASSERT_EQ(supply(6261, 0), PDK_BAD_PAYLOAD);
    ASSERT_EQ(r.payload[0], PDK_RAIL_VDD);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    uint8_t short_payload[2] = {0, 0};
    ASSERT_EQ(call(PDK_CMD_SUPPLY_SET, short_payload, 2), PDK_BAD_PAYLOAD);
}

TEST(zero_switches_off)
{
    calibrate();
    ASSERT_EQ(supply(5000, 11000), PDK_OK);
    ASSERT_EQ(supply(0, 0), PDK_OK);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(session_loss_removes_the_voltages)
{
    calibrate();
    ASSERT_EQ(supply(5000, 11000), PDK_OK);
    fake_set_open(false);
    for (int i = 0; i < 4; i++)
        pdk_core_poll();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(idle_timeout_removes_the_voltages_and_get_keeps_them)
{
    calibrate();
    ASSERT_EQ(supply(5000, 11000), PDK_OK);
    fake_advance_us(4u * 60u * 1000000u);
    ASSERT_EQ(call(PDK_CMD_SUPPLY_GET, NULL, 0), PDK_OK);   /* a monitoring client keeps them up */
    fake_advance_us(4u * 60u * 1000000u);
    pdk_core_poll();
    ASSERT_TRUE(fake_power_set_value(PDK_RAIL_VPP) > 0);
    fake_advance_us(60u * 1000000u + 1000u);
    pdk_core_poll();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(calibration_takes_over)
{
    calibrate();
    ASSERT_EQ(supply(3300, 0), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(supply(3300, 0), PDK_NOT_ALLOWED_NOW);
    uint8_t reject = 0;
    ASSERT_EQ(call(PDK_CMD_CAL_CONFIRM, &reject, 1), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_SUPPLY_GET, NULL, 0), PDK_OK);
    ASSERT_EQ(le16(r.payload + 0), 0);   /* the manual set point did not come back */
    ASSERT_EQ(supply(3300, 0), PDK_OK);
}
