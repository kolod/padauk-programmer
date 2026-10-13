/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supply calibration with an external meter (CAL_START / CAL_SET / CAL_CONFIRM / CAL_GET / CAL_CLEAR). */
#include "pdk_test.h"

#include <stdlib.h>

#include "fake_hal.h"
#include "pdk_core.h"
#include "power/power.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_proto.h"

static tp_frame_t r;
static uint16_t seq;

void pdk_test_setup(void)
{
    fake_reset();
    /* a board like the G474 one: unbuffered DAC with a pull-down (-5.5 %), 1 % divider resistors */
    fake_power_scale(PDK_RAIL_VDD, 945000, 1012000);
    fake_power_scale(PDK_RAIL_VPP, 948000, 988000);
    pdk_core_init();
    seq = 1;
    tp_hello(seq++);
}

static uint16_t call(uint16_t code, const void *p, uint16_t len)
{
    tp_call(seq++, code, p, len, &r);
    return r.status;
}

static uint16_t send_measured(uint16_t vdd, uint16_t vpp)
{
    uint8_t p[4];
    put16(p, vdd);
    put16(p + 2, vpp);
    return call(PDK_CMD_CAL_SET, p, 4);
}

static uint16_t confirm(uint8_t accept) { return call(PDK_CMD_CAL_CONFIRM, &accept, 1); }

static int pct_off(uint16_t value, uint16_t target)   /* deviation in 0.01 % */
{
    return (int)(((int32_t)value - target) * 10000 / target);
}

/* The whole flow, as pdkprog drives it: meter readings come from the fake's true voltage. */
static void calibrate_and_accept(void)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(r.len, 8);
    ASSERT_EQ(send_measured(fake_power_true_mV(PDK_RAIL_VDD), fake_power_true_mV(PDK_RAIL_VPP)), PDK_OK);
    ASSERT_EQ(confirm(1), PDK_OK);
}

TEST(without_calibration_supplies_fail_the_tolerance_check)
{
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VPP, 12000, 0, &f), PDK_VPP_FAULT);
}

TEST(info_reports_the_capability)
{
    ASSERT_EQ(call(PDK_CMD_GET_INFO, NULL, 0), PDK_OK);
    ASSERT_TRUE(le32(r.payload + 10) & PDK_CAP_SUPPLY_CAL);
}

TEST(start_raises_both_rails_to_90_percent_of_the_limits)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(le16(r.payload + 0), 6260u * 9u / 10u);    /* fake limits 6260 / 13270 mV */
    ASSERT_EQ(le16(r.payload + 2), 13270u * 9u / 10u);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 6260u * 9u / 10u);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 13270u * 9u / 10u);
    /* the raw reading is what the uncalibrated ADC shows */
    ASSERT_TRUE(abs(pct_off(le16(r.payload + 4), fake_power_true_mV(PDK_RAIL_VDD))) < 150);
}

TEST(set_gives_exact_6_and_12_volts)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(send_measured(fake_power_true_mV(PDK_RAIL_VDD), fake_power_true_mV(PDK_RAIL_VPP)), PDK_OK);
    ASSERT_EQ(r.len, 12);
    ASSERT_EQ(le16(r.payload + 0), 5947);   /* 6 V is above 95 % of the 6.26 V limit: clamped */
    ASSERT_EQ(le16(r.payload + 2), 12000);
    /* what a meter shows now is within 0.2 % of the targets */
    ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VDD), 5947)) <= 20);
    ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VPP), 12000)) <= 20);
    int8_t cal[4];
    memcpy(cal, r.payload + 8, 4);
    ASSERT_TRUE(abs(cal[POWER_CAL_OUT_VDD] + 55) <= 1);   /* -5.5 % */
    ASSERT_TRUE(abs(cal[POWER_CAL_OUT_VPP] + 52) <= 1);
    ASSERT_TRUE(abs(cal[POWER_CAL_MEAS_VDD] + 12) <= 1);  /* 1 / 1.012 */
    ASSERT_TRUE(abs(cal[POWER_CAL_MEAS_VPP] - 12) <= 1);
}

TEST(accept_persists_across_restart)
{
    calibrate_and_accept();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);   /* confirm removes the voltages */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    pdk_core_init();   /* power cycle */
    tp_hello(seq++);
    ASSERT_EQ(call(PDK_CMD_CAL_GET, NULL, 0), PDK_OK);
    ASSERT_EQ(r.payload[0], 1);
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VPP, 11000, 0, &f), PDK_OK);
    ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VPP), 11000)) <= 20);
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5800, 0, &f), PDK_OK);
    ASSERT_TRUE(abs(pct_off(fake_power_true_mV(PDK_RAIL_VDD), 5800)) <= 20);
}

TEST(reject_keeps_the_previous_calibration)
{
    calibrate_and_accept();
    ASSERT_EQ(call(PDK_CMD_CAL_GET, NULL, 0), PDK_OK);
    uint8_t before[5];
    memcpy(before, r.payload, 5);
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(send_measured(fake_power_true_mV(PDK_RAIL_VDD) + 100, fake_power_true_mV(PDK_RAIL_VPP)), PDK_OK);
    ASSERT_EQ(confirm(0), PDK_OK);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(call(PDK_CMD_CAL_GET, NULL, 0), PDK_OK);
    ASSERT_MEM_EQ(r.payload, before, 5);
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VDD, 5800, 0, &f), PDK_OK);   /* old coefficients in effect */
}

TEST(implausible_reading_is_rejected_and_can_be_retyped)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(send_measured(fake_power_true_mV(PDK_RAIL_VDD), 2000), PDK_BAD_PAYLOAD);   /* a typo */
    ASSERT_EQ(r.payload[0], PDK_RAIL_VPP);
    ASSERT_TRUE(fake_power_set_value(PDK_RAIL_VPP) > 0);   /* still raised */
    ASSERT_EQ(send_measured(fake_power_true_mV(PDK_RAIL_VDD), fake_power_true_mV(PDK_RAIL_VPP)), PDK_OK);
    ASSERT_EQ(confirm(1), PDK_OK);
}

TEST(sequence_is_enforced)
{
    ASSERT_EQ(send_measured(6000, 12000), PDK_NOT_ALLOWED_NOW);
    ASSERT_EQ(confirm(1), PDK_NOT_ALLOWED_NOW);
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(confirm(1), PDK_NOT_ALLOWED_NOW);   /* nothing to accept before CAL_SET */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);   /* … and the voltages are off */
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_CAL_CLEAR, NULL, 0), PDK_NOT_ALLOWED_NOW);
}

TEST(session_loss_removes_the_voltages)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    ASSERT_TRUE(fake_power_set_value(PDK_RAIL_VPP) > 0);
    fake_set_open(false);
    for (int i = 0; i < 4; i++)
        pdk_core_poll();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(idle_timeout_removes_the_voltages)
{
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_OK);
    fake_advance_us(5u * 60u * 1000000u + 1000u);
    pdk_core_poll();
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_EQ(send_measured(6000, 12000), PDK_NOT_ALLOWED_NOW);
}

TEST(dead_rail_is_a_fault_not_a_calibration)
{
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_STUCK);
    ASSERT_EQ(call(PDK_CMD_CAL_START, NULL, 0), PDK_VPP_FAULT);
    ASSERT_EQ(r.payload[0], PDK_RAIL_VPP);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(clear_returns_to_uncalibrated)
{
    calibrate_and_accept();
    ASSERT_EQ(call(PDK_CMD_CAL_CLEAR, NULL, 0), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_CAL_GET, NULL, 0), PDK_OK);
    ASSERT_EQ(r.payload[0], 0);
    pdk_core_init();
    power_fault_t f;
    ASSERT_EQ(power_apply(PDK_RAIL_VPP, 12000, 0, &f), PDK_VPP_FAULT);
}
