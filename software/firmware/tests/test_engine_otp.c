/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OTP writing on the fake IC (T032): PMS150C (OTP1_2) and PMS152 (OTP2_2) written block by block and read back, bits
 * never return to 1, GetID before every session (an IC swapped for another one is refused before the write command and
 * without the operation voltages), erase refused without any voltage, a write interrupted after N blocks finished by a
 * second session, the read-only families (OTP2_1 without write voltages, OTP3_1) refused without any voltage. */
#include "pdk_test.h"

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "engine_internal.h"
#include "fake_hal.h"
#include "fake_icp.h"
#include "fake_icp_ic.h"
#include "proto/proto_defs.h"

static uint16_t buf[0x1000];
static unsigned polls, cancel_after;

void pdk_test_setup(void)
{
    fake_reset();
    eng_ses.pause_us = 0;
    eng_ses.state = SES_IDLE;
    polls = 0;
    cancel_after = 0;
    fake_icp_record(false);
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

static uint16_t pattern(const chip_info_t *c, uint16_t a) { return (uint16_t)((a * 0x2F31u + 3u) & chip_word_mask(c)); }

static uint16_t write_range(const chip_info_t *c, uint16_t from, uint16_t to)
{
    for (uint16_t a = from; a < to; a += c->write_block) {
        uint16_t blk[16];
        for (unsigned i = 0; i < c->write_block; i++)
            blk[i] = pattern(c, (uint16_t)(a + i));
        uint16_t st = chip_engine_write_block(c, a, blk, c->write_block, poll);
        if (st != PDK_OK)
            return st;
    }
    return PDK_OK;
}

static void read_all(const chip_info_t *c)
{
    ASSERT_EQ(chip_engine_read(c, 0, c->code_words, sink, NULL, poll), PDK_OK);
    chip_engine_safe_state();
}

static void write_and_read_back(const char *name)
{
    const chip_info_t *c = chip(name);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(c->write_block, 2);
    fake_ic_insert(c);
    ASSERT_EQ(write_range(c, 0, 128), PDK_OK);
    ASSERT_EQ(fake_ic_enters(), 2u);   /* the GetID pass, then one write session for all the blocks */
    ASSERT_EQ(fake_ic_blocks(), 64u);
    chip_engine_safe_state();
    read_all(c);
    for (uint16_t a = 0; a < c->code_words; a++)
        ASSERT_EQ(buf[a], a < 128 ? pattern(c, a) : chip_word_mask(c));
    ASSERT_EQ(fake_ic_max_vpp(), c->vpp_write);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(pms150c_write_and_read_back) { write_and_read_back("PMS150C"); }
TEST(pms152_write_and_read_back) { write_and_read_back("PMS152"); }

TEST(bits_never_return_to_one)
{
    const chip_info_t *c = chip("PMS150C");
    fake_ic_insert(c);
    uint16_t a[2] = {0x1555, 0x0F0F}, b[2] = {0x0AAA, 0x1FFF};
    ASSERT_EQ(chip_engine_write_block(c, 0x20, a, 2, poll), PDK_OK);
    chip_engine_safe_state();
    ASSERT_EQ(chip_engine_write_block(c, 0x20, b, 2, poll), PDK_OK);   /* a second write only clears more bits */
    chip_engine_safe_state();
    read_all(c);
    ASSERT_EQ(buf[0x20], 0x1555 & 0x0AAA);
    ASSERT_EQ(buf[0x21], 0x0F0F);
}

TEST(getid_before_every_session_refuses_a_swapped_ic)
{
    const chip_info_t *c = chip("PMS152");
    fake_ic_insert(c);
    ASSERT_EQ(write_range(c, 0, 4), PDK_OK);
    chip_engine_safe_state();
    uint32_t enters = fake_ic_enters();
    fake_ic_set_id(0xA16);   /* a PMS150C in the socket now */
    uint16_t blk[2] = {0, 0};
    ASSERT_EQ(chip_engine_write_block(c, 4, blk, 2, poll), PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(fake_ic_enters(), enters + 1u);   /* the GetID pass only: no write session */
    ASSERT_EQ(fake_ic_blocks(), 2u);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(a_swapped_ic_never_sees_the_operation_voltages)
{
    const chip_info_t *c = chip("PMS150C");
    fake_ic_insert(c);
    fake_ic_set_id(0xA27);
    uint16_t blk[2] = {0, 0};
    ASSERT_EQ(chip_engine_write_block(c, 0, blk, 2, poll), PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(fake_ic_max_vpp(), c->vpp_cmd_write);
    ASSERT_EQ(fake_ic_max_vdd(), c->vdd_cmd_write);
    ASSERT_EQ(fake_ic_blocks(), 0u);
}

TEST(no_ic_is_not_found)
{
    const chip_info_t *c = chip("PMS150C");
    uint16_t blk[2] = {0, 0};
    ASSERT_EQ(chip_engine_write_block(c, 0, blk, 2, poll), PDK_CHIP_NOT_FOUND);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(erase_is_refused_without_any_voltage)
{
    const chip_info_t *c = chip("PMS150C");
    fake_ic_insert(c);
    ASSERT_EQ(chip_engine_erase(c, poll), PDK_OP_UNSUPPORTED);
    ASSERT_EQ(fake_ic_max_vpp(), 0);
    ASSERT_EQ(fake_ic_max_vdd(), 0);
}

TEST(an_interrupted_write_is_finished_by_a_second_session)
{
    const chip_info_t *c = chip("PMS152");
    fake_ic_insert(c);
    cancel_after = 25;   /* the pause after the GetID pass polls too (every 1 ms of its 10 ms) */
    ASSERT_EQ(write_range(c, 0, 64), PDK_CANCELLED);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);   /* left at once */
    ASSERT_TRUE(!fake_icp_driven(ICP_PA4, NULL));
    uint32_t done = fake_ic_blocks();
    ASSERT_TRUE(done > 0 && done < 32);
    cancel_after = 0;
    ASSERT_EQ(write_range(c, (uint16_t)(done * 2u), 64), PDK_OK);
    chip_engine_safe_state();
    read_all(c);
    for (uint16_t a = 0; a < c->code_words; a++)
        ASSERT_EQ(buf[a], a < 64 ? pattern(c, a) : chip_word_mask(c));
}

TEST(read_only_families_are_refused_without_any_voltage)
{
    static const char *names[] = {"PMS133", "PMC251"};   /* OTP2_1 without write voltages, OTP3_1 */
    for (size_t i = 0; i < 2; i++) {
        const chip_info_t *c = chip(names[i]);
        fake_ic_insert(c);
        uint16_t blk[16] = {0};
        ASSERT_EQ(chip_engine_write_block(c, 0, blk, c->write_block ? c->write_block : 2, poll), PDK_OP_UNSUPPORTED);
        ASSERT_EQ(fake_ic_max_vpp(), 0);
        ASSERT_EQ(fake_ic_max_vdd(), 0);
    }
}
