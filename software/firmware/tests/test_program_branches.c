/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PROGRAM branches on an OTP chip (PMS150C) and the fake engine: blank / compatible (retry) / already
 * programmed / incompatible, serial number, failed calibration, package corruption, final verify. */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"
#include "test_ops.h"

#define SITE 0x40u
#define SERIAL 0x60u

static tops_image_t img;
static const chip_info_t *chip;
static uint16_t expect[TOPS_MAX_WORDS];
static uint16_t before[TOPS_MAX_WORDS];

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    tops_seq = 1;
    chip = tops_chip("PMS150C");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, SITE, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_serial(&img, chip, SERIAL);
    tops_put_code(&img, chip, 0x70, 0x20);
    tops_put(&img, 0x3FF, 0x1FFD);   /* fuse */
    fake_chip_insert(chip);
}

static const op_result_t *program(uint8_t flags, uint8_t source)
{
    if (tops_upload(chip, &img) != TP_STAGE_OK)
        return NULL;
    return tops_run(PDK_OP_PROGRAM, flags, source, NULL);
}

static void snapshot(void) { memcpy(before, fake_chip_mem(), 2u * chip->code_words); }
static bool unchanged(void) { return !memcmp(before, fake_chip_mem(), 2u * chip->code_words); }

/* A fully programmed chip as PROGRAM leaves it (k = 0x37, given serial). */
static void programmed_chip(uint16_t *out)
{
    op_result_t r = {0};
    r.calib[0].value = 0x37;
    for (int i = 0; i < 8; i++)
        r.serial[i] = (uint8_t)(0xA1 + i);
    tops_expected(chip, &img, &r, out);
}

TEST(blank_otp)
{
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->warnings & PDK_WARN_RESUMED, 0);
    tops_expected(chip, &img, r, expect);
    ASSERT_MEM_EQ(fake_chip_mem(), expect, 2u * chip->code_words);
}

TEST(skip_blank_check_is_refused_for_otp)
{
    const op_result_t *r = program(PDK_OPF_SKIP_BLANK_CHECK, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_PKG_OTP_BLANK_CHECK_REQUIRED);
    ASSERT_EQ(fake_chip_writes(), 0);
}

TEST(compatible_otp_is_completed_and_keeps_its_serial)
{
    uint16_t *m = fake_chip_mem();
    programmed_chip(expect);
    for (unsigned a = 0; a < 0x30; a++)   /* interrupted earlier: part of the code, serial written */
        m[a] = expect[a];
    for (unsigned i = 0; i < 8; i++)
        m[SERIAL + i] = expect[SERIAL + i];
    calib_site_t s = {.addr = SITE, .kind = CALIB_KIND_IHRC, .reg0 = chip->reg_ihrcr};
    calib_loop_words(chip->code_bits, &s, CALIB_LOOP_IMM, m + SITE);   /* the loop is there, not finished */
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_TRUE(r->warnings & PDK_WARN_RESUMED);
    ASSERT_EQ(r->serial_valid, 1);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(r->serial[i], 0xA1 + i);   /* taken from the chip */
    tops_expected(chip, &img, r, expect);
    ASSERT_MEM_EQ(fake_chip_mem(), expect, 2u * chip->code_words);
}

TEST(fully_programmed_otp_is_left_alone)
{
    programmed_chip(fake_chip_mem());
    snapshot();
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CHIP_ALREADY_PROGRAMMED);
    ASSERT_EQ(fake_chip_writes(), 0);
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(journal_get()->ok_count, 0);
    ASSERT_EQ(journal_get()->fail_count, 0);
}

TEST(incompatible_otp_is_not_touched)
{
    uint16_t *m = fake_chip_mem();
    m[0x10] = (uint16_t)(img.w[0x10] & ~1u);   /* a 0 where the image needs a 1 */
    snapshot();
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_BLANK);
    ASSERT_EQ(r->stage, PDK_STAGE_BLANK_CHECK);
    ASSERT_EQ(r->fail_addr, 0x10);
    ASSERT_EQ(r->expected, img.w[0x10]);
    ASSERT_EQ(r->actual, m[0x10]);
    ASSERT_EQ(fake_chip_writes(), 0);
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(journal_get()->fail_count, 1);
}

TEST(half_written_serial_is_incompatible)
{
    uint16_t *m = fake_chip_mem();
    m[SERIAL] = (uint16_t)(0x0100u | 0x12);   /* one RET #imm, the rest blank */
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_BLANK);
    ASSERT_EQ(r->fail_addr, SERIAL + 1);
    ASSERT_EQ(fake_chip_writes(), 0);
}

TEST(new_serial_for_every_chip)
{
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_OK);
    uint8_t first[8];
    memcpy(first, r->serial, 8);
    uint32_t seq1 = journal_get()->seq;
    fake_chip_insert(chip);
    r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_TRUE(memcmp(first, r->serial, 8) != 0);
    ASSERT_TRUE(journal_get()->seq > seq1);   /* the number was spent in the journal before writing */
}

TEST(broken_entropy_source_writes_nothing)
{
    fake_entropy(FAKE_ENTROPY_CONSTANT, 0);
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_INTERNAL);
    ASSERT_EQ(fake_chip_writes(), 0);
}

TEST(failed_calibration_stops_before_anything_else)
{
    fake_chip_calib(FAKE_CAL_FAIL, 0x52);
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CALIB_FAILED);
    ASSERT_EQ(r->stage, PDK_STAGE_CALIBRATE);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(r->calib[0].value, 0x52);
    ASSERT_EQ(r->fuse_written, 0xFFFF);
    calib_match_t m;
    ASSERT_TRUE(calib_match(chip, fake_chip_mem() + SITE, 14, SITE, &m));
    ASSERT_EQ(m.form, CALIB_FORM_LOOP);                    /* left as a loop for a later retry */
    ASSERT_EQ(fake_chip_mem()[0x3FF], 0x1FFF);             /* no fuse */
    ASSERT_EQ(journal_get()->fail_count, 1);
}

TEST(finalize_on_fail_uses_the_best_value_from_the_host_only)
{
    fake_chip_calib(FAKE_CAL_FAIL, 0x52);
    const op_result_t *r = program(PDK_OPF_CALIB_FINALIZE_ON_FAIL, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CALIB_FAILED);
    ASSERT_EQ(r->stage, PDK_STAGE_FINAL_VERIFY);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FORCED);
    ASSERT_TRUE(r->warnings & PDK_WARN_CALIB_FORCED);
    ASSERT_EQ(r->fuse_written, 0x1FFD);
    tops_expected(chip, &img, r, expect);
    ASSERT_MEM_EQ(fake_chip_mem(), expect, 2u * chip->code_words);

    fake_chip_insert(chip);   /* local start ignores the flag */
    fake_chip_calib(FAKE_CAL_FAIL, 0x52);
    r = tops_run(PDK_OP_PROGRAM, PDK_OPF_CALIB_FINALIZE_ON_FAIL, PDK_SOURCE_LOCAL, NULL);
    ASSERT_EQ(r->status, PDK_CALIB_FAILED);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(r->fuse_written, 0xFFFF);
}

static void corrupt(uint32_t offset)
{
    uint32_t base = package_store_active_slot() == 0 ? 0x0800B000u : 0x0800D000u;
    *fake_flash_mem(base + offset) ^= 0x01;
}

static bool corrupted;

static void corrupt_image_in_write(void)
{
    if (op_current()->stage == PDK_STAGE_WRITE && !corrupted) {
        corrupt(128u + 2u * 0x200u);   /* an unset word far behind the cached part of the package */
        corrupted = true;
    }
}

TEST(package_corrupted_while_writing_stops_before_the_fuse)
{
    corrupted = false;
    fake_chip_hook(corrupt_image_in_write);
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_STORE_CORRUPTED);
    ASSERT_EQ(r->stage, PDK_STAGE_WRITE);
    ASSERT_EQ(fake_chip_mem()[0x3FF], 0x1FFF);
}

static void corrupt_header_in_verify(void)
{
    if (op_current()->stage == PDK_STAGE_VERIFY && !corrupted) {
        corrupt(100);   /* the label: the image itself is still right */
        corrupted = true;
    }
}

TEST(package_check_before_the_fuse)
{
    corrupted = false;
    fake_chip_hook(corrupt_header_in_verify);
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_STORE_CORRUPTED);
    ASSERT_EQ(r->stage, PDK_STAGE_PKG_CHECK);
    ASSERT_EQ(r->fuse_written, 0xFFFF);
    ASSERT_EQ(fake_chip_mem()[0x3FF], 0x1FFF);
}

TEST(final_verify_catches_a_weak_word)
{
    fake_chip_weak(SITE + 2);   /* the loop's MOV A,#FF sticks, the k written later does not */
    fake_chip_calib(FAKE_CAL_OK, 0x37);
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_VERIFY_FAILED);
    ASSERT_EQ(r->stage, PDK_STAGE_FINAL_VERIFY);
    ASSERT_EQ(r->fail_addr, SITE + 2);
    ASSERT_EQ(r->actual & 0xFF, 0xFF);
    ASSERT_EQ(r->expected & 0xFF, 0x37);
}

TEST(wrong_chip_and_no_chip)
{
    fake_chip_set_id(0xAA1);   /* a PFS154 in the socket */
    const op_result_t *r = program(0, PDK_SOURCE_HOST);
    ASSERT_EQ(r->status, PDK_CHIP_ID_MISMATCH);
    ASSERT_EQ(r->stage, PDK_STAGE_ID);
    ASSERT_EQ(r->chip_id_found, 0xAA1);
    ASSERT_EQ(fake_chip_writes(), 0);
    fake_chip_remove();
    r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_FOUND);
}

TEST(no_package)
{
    tp_hello(tops_seq++);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_STORE_EMPTY);
    ASSERT_EQ(r->stage, PDK_STAGE_PKG_CHECK);
}
