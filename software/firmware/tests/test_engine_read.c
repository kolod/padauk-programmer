/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The chip engine on the fake IC lines (T012, T015): the session rules (VPP before VDD with the minimum pauses, lines
 * released before the supplies drop, the lazy post-leave pause, session reuse), probe of every family at the
 * identification levels, read of FLASH / OTP / OTP3_1 ICs, a missing IC and a wrong ID without programming voltages,
 * cancel through the safe state. */
#include "pdk_test.h"

#include <stdio.h>

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "engine_internal.h"
#include "fake_hal.h"
#include "fake_icp.h"
#include "fake_icp_ic.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "proto/trace.h"
#include "test_proto.h"

static uint16_t buf[0x1000];
static unsigned polls, cancel_after;

void pdk_test_setup(void)
{
    fake_reset();
    eng_ses.pause_us = 0;
    eng_ses.state = SES_IDLE;
    polls = 0;
    cancel_after = 0;
}

static bool poll(void) { return !cancel_after || ++polls < cancel_after; }
static void sink(void *ctx, uint16_t addr, uint16_t word) { (void)ctx, buf[addr] = word; }

static const chip_info_t *chip(const char *name)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), name))
            return chip_at(i);
    return NULL;
}

static void fill(const chip_info_t *c)
{
    for (unsigned a = 0; a < c->code_words; a++)
        fake_ic_mem()[a] = (uint16_t)((a * 0x1235u + 7u) & chip_word_mask(c));
}

static const fake_ev_t *first(fake_ev_kind_t kind, size_t *index)
{
    const fake_ev_t *ev;
    size_t n = fake_icp_events(&ev);
    for (size_t i = 0; i < n; i++)
        if (ev[i].kind == kind) {
            if (index)
                *index = i;
            return &ev[i];
        }
    return NULL;
}

TEST(enter_applies_vpp_before_vdd_with_the_minimum_pauses)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_OK);
    const fake_ev_t *ev;
    size_t n = fake_icp_events(&ev), ivpp, ivdd;
    ASSERT_TRUE(first(FEV_VPP, &ivpp) != NULL);
    ASSERT_TRUE(first(FEV_VDD, &ivdd) != NULL);
    ASSERT_TRUE(ivpp < ivdd);
    ASSERT_EQ(ev[ivpp].mV, c->vpp_cmd_read);
    ASSERT_EQ(ev[ivdd].mV, c->vdd_cmd_read);
    ASSERT_TRUE(ev[ivdd].t_us - ev[ivpp].t_us >= 100u);
    ASSERT_TRUE(ivdd + 1 < n && ev[ivdd + 1].t_us - ev[ivdd].t_us >= 500u);
    /* leave: lines idle first, then both supplies 0, immediately */
    fake_icp_clear();
    uint32_t t0 = fake_now_us();
    chip_engine_safe_state();
    n = fake_icp_events(&ev);
    ASSERT_TRUE(n >= 3);
    ASSERT_EQ(ev[0].kind, FEV_IDLE);
    ASSERT_TRUE(ev[1].kind == FEV_VDD || ev[1].kind == FEV_VPP);
    ASSERT_EQ(ev[1].mV, 0);
    ASSERT_EQ(ev[2].mV, 0);
    ASSERT_TRUE(fake_now_us() - t0 < 100u);   /* the safe state does not wait */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(the_post_leave_pause_is_charged_to_the_next_enter)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_OK);
    chip_engine_safe_state();
    uint32_t left = fake_now_us();
    fake_icp_clear();
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_OK);
    ASSERT_TRUE(first(FEV_VPP, NULL)->t_us - left >= 10000u);
}

TEST(consecutive_reads_share_one_session)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    fill(c);
    ASSERT_EQ(chip_engine_read(c, 0, 16, sink, NULL, poll), PDK_OK);
    ASSERT_EQ(chip_engine_read(c, 100, 16, sink, NULL, poll), PDK_OK);
    ASSERT_EQ(fake_ic_enters(), 1u);
    ASSERT_EQ(buf[100], fake_ic_mem()[100]);
    chip_engine_safe_state();
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_OK);
    ASSERT_EQ(fake_ic_enters(), 2u);   /* left in between: a new session */
}

TEST(probe_identifies_every_family_at_the_probe_levels)
{
    static const char *names[] = {"PFS154", "PFS173", "PFS172", "PMS150C", "PMS152", "PMS171B", "PMS133", "PMS154B",
                                  "PMC251"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        pdk_test_setup();
        const chip_info_t *c = chip(names[i]);
        fake_ic_insert(c);
        uint16_t id = 0;
        uint8_t fam = 0;
        uint16_t pst = chip_engine_probe(&id, &fam, poll);
        if (pst != PDK_OK)
            fprintf(stderr, "probe %s: 0x%04X\n", names[i], pst);
        ASSERT_EQ(pst, PDK_OK);
        ASSERT_EQ(id, c->id12);
        ASSERT_EQ(fam, c->family);
        ASSERT_TRUE(fake_ic_max_vpp() <= ENG_PROBE_VPP_MV);
        ASSERT_TRUE(fake_ic_max_vdd() <= ENG_PROBE_VDD_MV);
        ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);   /* left after probing */
    }
}

TEST(probe_of_an_empty_socket)
{
    uint16_t id = 1;
    uint8_t fam = 1;
    ASSERT_EQ(chip_engine_probe(&id, &fam, poll), PDK_CHIP_NOT_FOUND);
    ASSERT_EQ(id, 0);
    ASSERT_TRUE(fake_ic_max_vpp() <= ENG_PROBE_VPP_MV);
    ASSERT_TRUE(fake_ic_max_vdd() <= ENG_PROBE_VDD_MV);
}

TEST(read_returns_the_memory_of_each_family)
{
    static const char *names[] = {"PFS154", "PFS173", "PFS172", "PMS150C", "PMS152", "PMS133", "PMC251"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        pdk_test_setup();
        const chip_info_t *c = chip(names[i]);
        fake_ic_insert(c);
        fill(c);
        fake_icp_record(false);
        ASSERT_EQ(chip_engine_read(c, 0, c->code_words, sink, NULL, poll), PDK_OK);
        for (unsigned a = 0; a < c->code_words; a++)
            ASSERT_EQ(buf[a], fake_ic_mem()[a]);
        ASSERT_TRUE(fake_ic_max_vpp() <= (c->vpp_cmd_read > c->vpp_read ? c->vpp_cmd_read : c->vpp_read));
        chip_engine_safe_state();
    }
}

TEST(a_missing_ic_and_a_wrong_id_get_no_operation_voltage)
{
    const chip_info_t *c = chip("PFS154");
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_CHIP_NOT_FOUND);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    fake_ic_insert(c);
    fake_ic_set_id(0xAA2);
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    const chip_info_t *otp = chip("PMS150C");
    pdk_test_setup();
    fake_ic_insert(otp);
    fake_ic_set_id(0xA17);
    ASSERT_EQ(chip_engine_read(otp, 0, 1, sink, NULL, poll), PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(fake_ic_enters(), 1u);   /* only the GetID pass: the read session never started */
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

/* Protocol 1.1 trace: the engine reports entering, the raw ACK, and leaving (decoded from the EVT_TRACE frames). */
TEST(the_engine_traces_its_steps)
{
    pdk_core_init();
    tp_hello(1);
    trace_set(true);
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    ASSERT_EQ(chip_engine_read(c, 0, 1, sink, NULL, poll), PDK_OK);
    chip_engine_safe_state();
    tp_frame_t frames[64];
    int n = tp_collect(frames, 64);
    uint8_t kinds[64];
    int nk = 0;
    uint32_t ack = 0;
    for (int i = 0; i < n; i++)
        if (frames[i].type == PDK_FT_EVT && frames[i].code == PDK_EVT_TRACE && nk < 64) {
            kinds[nk++] = frames[i].payload[4];
            if (frames[i].payload[4] == TRACE_ACK)
                ack = le32(frames[i].payload + 10);
        }
    trace_set(false);
    /* VPP, VDD (power), ENTER, ACK, LEAVE */
    ASSERT_TRUE(nk >= 5);
    ASSERT_EQ(kinds[0], TRACE_POWER_OK);
    ASSERT_EQ(kinds[1], TRACE_POWER_OK);
    ASSERT_EQ(kinds[2], TRACE_ENTER);
    ASSERT_EQ(kinds[3], TRACE_ACK);
    ASSERT_EQ(kinds[nk - 1], TRACE_LEAVE);
    ASSERT_EQ(ack & 0xFFFu, 0xAA1u);
}

TEST(cancel_mid_read_ends_in_the_safe_state)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    cancel_after = 5;
    fake_icp_record(false);
    ASSERT_EQ(chip_engine_read(c, 0, c->code_words, sink, NULL, poll), PDK_CANCELLED);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_TRUE(!fake_icp_driven(ICP_CLK, NULL) && !fake_icp_driven(ICP_DAT, NULL));
}
