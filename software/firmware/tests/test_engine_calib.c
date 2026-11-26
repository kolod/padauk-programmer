/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Calibration on the fake counter (T027): the IHRC search finds the closest trim value and stops past the target, ILRC
 * steps of 0x10, BandGap the first value where PA0 reads 0, SNAP_STANDARD to the nearest standard within 10 % (else the
 * master's value), MEASURE_MASTER without pulses, and the FR-055 refusals (first reading out of ¼…4, no response to the
 * steps, PA0 not driven), an unreachable target, no loop running, the VDD pin limit, cancel. */
#include "pdk_test.h"

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "chips/calib_sites.h"
#include "chips/chip_table.h"
#include "engine_internal.h"
#include "fake_hal.h"
#include "fake_icp.h"
#include "fake_icp_ic.h"
#include "proto/proto_defs.h"

static const chip_info_t *pfs154;
static uint32_t base;            /* SYSCLK(k) = base · (0.75 + k / 512), as an IHRC trim curve */
static unsigned polls, cancel_after;

void pdk_test_setup(void)
{
    fake_reset();
    eng_ses.pause_us = 0;
    eng_ses.state = SES_IDLE;
    fake_icp_record(false);
    polls = 0;
    cancel_after = 0;
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), "PFS154"))
            pfs154 = chip_at(i);
}

static bool poll(void) { return !cancel_after || ++polls < cancel_after; }
static uint32_t linear(uint8_t k) { return (uint32_t)((uint64_t)base * (384u + k) / 512u); }
static uint32_t flat(uint8_t k) { return (void)k, base; }
static uint32_t none(uint8_t k) { return (void)k, 0; }

static unsigned closest(uint32_t target, unsigned last, unsigned step)
{
    unsigned best = 0;
    for (unsigned k = 0; k <= last; k += step) {
        uint32_t d = linear((uint8_t)k) > target ? linear((uint8_t)k) - target : target - linear((uint8_t)k);
        uint32_t b = linear((uint8_t)best) > target ? linear((uint8_t)best) - target : target - linear((uint8_t)best);
        if (d < b)
            best = k;
    }
    return best;
}

static uint16_t cal(uint8_t kind, uint32_t target, uint16_t mV, uint8_t *v, uint32_t *hz)
{
    return chip_engine_calibrate(pfs154, 0x40, kind, target, mV, v, hz, poll);
}

TEST(ihrc_finds_the_closest_value_and_stops_past_the_target)
{
    base = 8240000;   /* an oscillator 3 % fast */
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_OK);
    ASSERT_EQ(v, closest(8000000, 0x9F, 1));
    ASSERT_TRUE(hz > 7960000 && hz < 8040000);
    ASSERT_TRUE(fake_icp_calib_trim() < 0x9F);   /* stopped once past the target */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);   /* VDD off afterwards */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(ihrc_at_1_mhz)
{
    base = 1050000;
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 1000000, 4000, &v, &hz), PDK_OK);
    ASSERT_EQ(v, closest(1000000, 0x9F, 1));
}

TEST(ilrc_steps_of_0x10)
{
    base = 66000;
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_ILRC, 62500, 3300, &v, &hz), PDK_OK);
    ASSERT_EQ(v % 0x10, 0);
    ASSERT_EQ(v, closest(62500, 0xF0, 0x10));
}

TEST(bandgap_takes_the_first_value_where_pa0_reads_0)
{
    base = 1000000;
    fake_icp_calib_curve(linear);
    fake_icp_calib_bandgap(0x55);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_BG, 0, 3950, &v, &hz), PDK_OK);
    ASSERT_EQ(v, 0x60);
}

TEST(bandgap_with_pa0_not_driven_is_the_wrong_site)
{
    base = 1000000;
    fake_icp_calib_curve(linear);
    fake_icp_calib_bandgap(-1);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_BG, 0, 3950, &v, &hz), PDK_CALIB_WRONG_SITE);
}

TEST(a_first_reading_out_of_range_is_the_wrong_site)
{
    base = 100000000;   /* 75 MHz at k = 0: not the loop of an 8 MHz stub */
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_CALIB_WRONG_SITE);
    ASSERT_EQ(fake_icp_calib_trim(), 0);   /* refused before any step */
}

TEST(no_response_to_the_steps_is_the_wrong_site)
{
    base = 7000000;
    fake_icp_calib_curve(flat);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_CALIB_WRONG_SITE);
}

TEST(an_unreachable_target_fails_with_the_best_value)
{
    base = 4000000;   /* tops out at about 4.2 MHz */
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_CALIB_FAILED);
    ASSERT_EQ(v, 0x9F);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
}

TEST(no_loop_running_fails_within_a_second)
{
    fake_icp_calib_curve(none);
    uint8_t v;
    uint32_t hz;
    uint32_t t0 = fake_now_us();
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_CALIB_FAILED);
    ASSERT_TRUE(fake_now_us() - t0 < 1500000u);
}

TEST(snap_standard_calibrates_to_the_nearest_standard)
{
    base = 7900000;
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, CALIB_SNAP_TAG | 0x70, 4000, &v, &hz), PDK_OK);
    unsigned want = closest(8000000, 0xFF, 1);   /* two neighbours can be equally close: the counter's resolution decides */
    ASSERT_TRUE(v + 1u >= want && v <= want + 1u);
    ASSERT_TRUE(hz > 7960000 && hz < 8040000);
}

TEST(snap_standard_keeps_the_master_value_when_too_far)
{
    base = 6000000;   /* at kk the copy runs at about 5.8 MHz: no standard within 10 % */
    fake_icp_calib_curve(linear);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, CALIB_SNAP_TAG | 0x70, 4000, &v, &hz), PDK_OK);
    ASSERT_EQ(v, 0x70);
    ASSERT_TRUE(hz > linear(0x70) - 10000 && hz < linear(0x70) + 10000);
}

TEST(measure_freq_measures_without_pulses)
{
    base = 8240000;
    fake_icp_calib_curve(linear);
    uint32_t hz;
    ASSERT_EQ(chip_engine_measure_freq(pfs154, 0x40, CALIB_KIND_IHRC, 0x71, 4000, &hz, poll), PDK_OK);
    ASSERT_EQ(fake_icp_calib_trim(), 0);
    ASSERT_TRUE(hz > linear(0) - 5000 && hz < linear(0) + 5000);
    fake_icp_calib_curve(none);
    ASSERT_EQ(chip_engine_measure_freq(pfs154, 0x40, CALIB_KIND_IHRC, 0x71, 4000, &hz, poll), PDK_CALIB_FAILED);
}

TEST(vdd_above_the_pin_limit_is_refused_without_voltage)
{
    base = 8000000;
    fake_icp_calib_curve(linear);
    fake_icp_vdd_limit(5000);
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 5500, &v, &hz), PDK_OP_UNSUPPORTED);
    ASSERT_EQ(fake_ic_max_vdd(), 0);
}

TEST(cancel_during_the_search)
{
    base = 8240000;
    fake_icp_calib_curve(linear);
    cancel_after = 10;
    uint8_t v;
    uint32_t hz;
    ASSERT_EQ(cal(CALIB_KIND_IHRC, 8000000, 4000, &v, &hz), PDK_CANCELLED);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_TRUE(!fake_icp_driven(ICP_PA4, NULL));
}
