/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FR-055 in PROGRAM: the engine reports that the chip runs another placeholder than the one being
 * calibrated (CALIB_WRONG_SITE) → nothing more is written and CALIB_FINALIZE_ON_FAIL does not apply.
 * How the engine detects it is tested with the chip model (T133). */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "test_ops.h"

#define IHRC_SITE 0x40u
#define ILRC_SITE 0x60u

static tops_image_t img;
static const chip_info_t *chip;
static uint16_t expect[TOPS_MAX_WORDS];

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    tops_seq = 1;
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, IHRC_SITE, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_calib(&img, chip, ILRC_SITE, CALIB_KIND_ILRC, 62500, 4000);
    tops_put_code(&img, chip, 0x70, 0x10);
    tops_put(&img, 0x7FF, 0x3FFD);
    fake_chip_insert(chip);
}

static uint8_t form_at(uint16_t addr)
{
    calib_match_t m;
    if (!calib_match(chip, fake_chip_mem() + addr, 14, addr, &m))
        return 0;
    return m.form;
}

static const op_result_t *program(uint8_t flags)
{
    if (tops_upload(chip, &img) != TP_STAGE_OK)
        return NULL;
    return tops_run(PDK_OP_PROGRAM, flags, PDK_SOURCE_HOST, NULL);
}

TEST(wrong_site_at_the_first_measurement_writes_nothing)
{
    fake_chip_calib(FAKE_CAL_WRONG_SITE, 0);
    const op_result_t *r = program(0);
    ASSERT_EQ(r->status, PDK_CALIB_WRONG_SITE);
    ASSERT_EQ(r->stage, PDK_STAGE_CALIBRATE);
    ASSERT_EQ(r->calib_count, 2);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(r->calib[0].measured_hz, 8000000u * 100u);
    ASSERT_EQ(r->calib[1].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(form_at(IHRC_SITE), CALIB_FORM_LOOP);
    ASSERT_EQ(form_at(ILRC_SITE), CALIB_FORM_LOOP);
    ASSERT_EQ(fake_chip_mem()[0x7FF], 0x3FFF);
    ASSERT_EQ(fake_chip_calibrations(), 1);
}

TEST(finalize_on_fail_does_not_apply)
{
    fake_chip_calib(FAKE_CAL_WRONG_SITE, 0x33);
    const op_result_t *r = program(PDK_OPF_CALIB_FINALIZE_ON_FAIL);
    ASSERT_EQ(r->status, PDK_CALIB_WRONG_SITE);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(r->warnings & PDK_WARN_CALIB_FORCED, 0);
    ASSERT_EQ(form_at(IHRC_SITE), CALIB_FORM_LOOP);
    ASSERT_EQ(fake_chip_mem()[0x7FF], 0x3FFF);
}

TEST(wrong_site_at_the_second_placeholder_keeps_the_first)
{
    fake_chip_calib(FAKE_CAL_OK, 0x5A);
    fake_chip_calib(FAKE_CAL_WRONG_SITE, 0);
    const op_result_t *r = program(0);
    ASSERT_EQ(r->status, PDK_CALIB_WRONG_SITE);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_OK);
    ASSERT_EQ(r->calib[1].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(form_at(IHRC_SITE), CALIB_FORM_FOOTPRINT);
    ASSERT_EQ(form_at(ILRC_SITE), CALIB_FORM_LOOP);
    ASSERT_EQ(fake_chip_mem()[0x7FF], 0x3FFF);
}

TEST(right_order_calibrates_both)
{
    fake_chip_calib(FAKE_CAL_OK, 0x5A);
    fake_chip_calib(FAKE_CAL_OK, 0x21);
    const op_result_t *r = program(0);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->calib[0].value, 0x5A);
    ASSERT_EQ(r->calib[1].value, 0x21);
    tops_expected(chip, &img, r, expect);
    ASSERT_MEM_EQ(fake_chip_mem(), expect, 2u * chip->code_words);
}
