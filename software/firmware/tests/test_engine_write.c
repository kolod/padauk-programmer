/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FLASH erase and write on the fake IC (T022): erase then blank, blocks written and read back (PFS154, PFS172), only
 * 1 → 0 changes, consecutive blocks in one session, a read in between leaves and re-enters, a weak cell shows on
 * read-back, cancel mid-erase / mid-write ends safely, an IC swapped between blocks is refused, OTP erase refused
 * without any voltage. */
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

/* Writes [0, words) block by block and reads everything back. */
static void write_and_check(const chip_info_t *c, uint16_t words)
{
    for (uint16_t a = 0; a < words; a += c->write_block) {
        uint16_t blk[16];
        for (unsigned i = 0; i < c->write_block; i++)
            blk[i] = pattern(c, (uint16_t)(a + i));
        ASSERT_EQ(chip_engine_write_block(c, a, blk, c->write_block, poll), PDK_OK);
    }
    ASSERT_EQ(chip_engine_read(c, 0, c->code_words, sink, NULL, poll), PDK_OK);
    for (uint16_t a = 0; a < c->code_words; a++)
        ASSERT_EQ(buf[a], a < words ? pattern(c, a) : chip_word_mask(c));
    chip_engine_safe_state();
}

TEST(erase_then_write_and_read_back_pfs154)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    for (unsigned a = 0; a < c->code_words; a++)
        fake_ic_mem()[a] = 0;   /* fully programmed: erase must bring every bit back */
    ASSERT_EQ(chip_engine_erase(c, poll), PDK_OK);
    ASSERT_EQ(fake_ic_erases(), 1u);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);   /* left after erasing */
    write_and_check(c, 256);
}

TEST(erase_then_write_and_read_back_pfs172)
{
    const chip_info_t *c = chip("PFS172");   /* address before data, VPP 0 in the operation */
    fake_ic_insert(c);
    for (unsigned a = 0; a < c->code_words; a++)
        fake_ic_mem()[a] = 0;
    ASSERT_EQ(chip_engine_erase(c, poll), PDK_OK);
    write_and_check(c, 64);
}

TEST(consecutive_blocks_share_one_session)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    uint16_t blk[4] = {1, 2, 3, 4};
    for (uint16_t a = 0; a < 64; a += 4)
        ASSERT_EQ(chip_engine_write_block(c, a, blk, 4, poll), PDK_OK);
    ASSERT_EQ(fake_ic_enters(), 1u);
    ASSERT_EQ(fake_ic_blocks(), 16u);
    ASSERT_EQ(chip_engine_read(c, 0, 4, sink, NULL, poll), PDK_OK);   /* another mode: leave, re-enter */
    ASSERT_EQ(fake_ic_enters(), 2u);
    ASSERT_EQ(chip_engine_write_block(c, 64, blk, 4, poll), PDK_OK);
    ASSERT_EQ(fake_ic_enters(), 3u);
}

TEST(writing_only_clears_bits)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    uint16_t first[4] = {0x0F0F, 0x3FFF, 0x3FFF, 0x3FFF}, second[4] = {0x30F0, 0x3FFF, 0x3FFF, 0x3FFF};
    ASSERT_EQ(chip_engine_write_block(c, 8, first, 4, poll), PDK_OK);
    ASSERT_EQ(chip_engine_write_block(c, 8, second, 4, poll), PDK_OK);
    ASSERT_EQ(fake_ic_mem()[8], 0x0000);   /* without an erase the second write only adds zeros */
}

TEST(a_weak_cell_shows_on_read_back)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    fake_ic_weak(0x21);
    uint16_t a[4] = {0x3FF0, 0x3FF0, 0x3FF0, 0x3FF0}, b[4] = {0x3F00, 0x3F00, 0x3F00, 0x3F00};
    ASSERT_EQ(chip_engine_write_block(c, 0x20, a, 4, poll), PDK_OK);
    ASSERT_EQ(chip_engine_write_block(c, 0x20, b, 4, poll), PDK_OK);   /* the second write does not stick at 0x21 */
    ASSERT_EQ(chip_engine_read(c, 0x20, 4, sink, NULL, poll), PDK_OK);
    ASSERT_EQ(buf[0x20], 0x3F00);
    ASSERT_EQ(buf[0x21], 0x3FF0);
}

TEST(an_ic_swapped_between_blocks_is_refused)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    uint16_t blk[4] = {1, 2, 3, 4};
    ASSERT_EQ(chip_engine_write_block(c, 0, blk, 4, poll), PDK_OK);
    chip_engine_safe_state();   /* the pipeline does something else in between */
    fake_ic_insert(chip("PFS173"));
    ASSERT_EQ(chip_engine_write_block(c, 4, blk, 4, poll), PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
}

TEST(cancel_mid_erase_and_mid_write)
{
    const chip_info_t *c = chip("PFS154");
    fake_ic_insert(c);
    cancel_after = 2;
    ASSERT_EQ(chip_engine_erase(c, poll), PDK_CANCELLED);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
    ASSERT_TRUE(!fake_icp_driven(ICP_CLK, NULL));
    polls = 0;
    cancel_after = 4;
    uint16_t blk[4] = {1, 2, 3, 4};
    uint16_t st = PDK_OK;
    for (uint16_t a = 0; a < 64 && st == PDK_OK; a += 4)
        st = chip_engine_write_block(c, a, blk, 4, poll);
    ASSERT_EQ(st, PDK_CANCELLED);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

TEST(otp_erase_is_refused_without_any_voltage)
{
    const chip_info_t *c = chip("PMS150C");
    fake_ic_insert(c);
    ASSERT_EQ(chip_engine_erase(c, poll), PDK_OP_UNSUPPORTED);
    ASSERT_EQ(fake_ic_max_vpp(), 0);
    ASSERT_EQ(fake_ic_max_vdd(), 0);
}
