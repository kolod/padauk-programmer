/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PROBE, ERASE, BLANK_CHECK and VERIFY (FR-047) on the fake chip engine, and the OP_* / GET_STATUS
 * commands. */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_ops.h"

#define SITE 0x40u
#define SERIAL 0x60u

static tops_image_t img;
static const chip_info_t *chip;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    pdk_core_poll();   /* the start-up SELF_TEST */
    tops_seq = 1;
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, SITE, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_serial(&img, chip, SERIAL);
    tops_put_code(&img, chip, 0x70, 0x10);
    tops_put(&img, 0x7FF, 0x3FFD);
    fake_chip_insert(chip);
}

static op_result_t programmed;

static void program_chip(void)
{
    fake_chip_calib(FAKE_CAL_OK, 0x5A);
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    programmed = *r;
}

TEST(probe)
{
    const op_result_t *r = tops_run(PDK_OP_PROBE, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->chip_id_found, chip->id12);
    ASSERT_EQ(r->family_found, chip->family);
    fake_chip_remove();
    r = tops_run(PDK_OP_PROBE, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_FOUND);
}

TEST(erase_then_blank)
{
    program_chip();
    op_request_t req = {.op_type = PDK_OP_ERASE, .chip_id12 = chip->id12};
    const op_result_t *r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_BLANK_CHECK);
    for (unsigned a = 0; a < chip->code_words; a++)
        ASSERT_EQ(fake_chip_mem()[a], 0x3FFF);
    req.chip_id12 = 0;   /* type from the active package */
    r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_OK);
}

TEST(erase_is_for_flash_only)
{
    const chip_info_t *otp = tops_chip("PMS150C");
    fake_chip_insert(otp);
    op_request_t req = {.op_type = PDK_OP_ERASE, .chip_id12 = otp->id12};
    const op_result_t *r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_OP_UNSUPPORTED);
    ASSERT_EQ(fake_chip_erases(), 0);
}

TEST(blank_check_points_at_the_first_written_word)
{
    fake_chip_mem()[0x123] = 0x1234;
    op_request_t req = {.op_type = PDK_OP_BLANK_CHECK, .chip_id12 = chip->id12};
    const op_result_t *r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_BLANK);
    ASSERT_EQ(r->fail_addr, 0x123);
    ASSERT_EQ(r->expected, 0x3FFF);
    ASSERT_EQ(r->actual, 0x1234);
    fake_chip_mem()[0x123] = 0x3FFF;
    fake_chip_mem()[0x7E5] = 0x0000;   /* reserved area: not looked at */
    r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    req.chip_id12 = 0;                 /* no package to take the type from */
    r = tops_run_req(&req, NULL);
    ASSERT_EQ(r->status, PDK_STORE_EMPTY);
}

TEST(verify_accepts_the_chip_own_calibration_and_serial)
{
    program_chip();
    const op_result_t *r = tops_run(PDK_OP_VERIFY, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->calib[0].value, 0x5A);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_OK);
    ASSERT_EQ(r->serial_valid, 1);
    ASSERT_MEM_EQ(r->serial, programmed.serial, 8);
    ASSERT_EQ(r->image_crc_raw, programmed.image_crc_raw);
    ASSERT_EQ(r->image_crc_norm, programmed.image_crc_norm);

    uint16_t *m = fake_chip_mem();   /* another chip of the batch: other k, other serial */
    m[SITE + 2] = (uint16_t)((m[SITE + 2] & 0xFF00u) | 0x21);
    m[SERIAL + 3] = (uint16_t)((m[SERIAL + 3] & 0xFF00u) | 0x99);
    r = tops_run(PDK_OP_VERIFY, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->calib[0].value, 0x21);
    ASSERT_EQ(r->serial[3], 0x99);
    ASSERT_EQ(r->image_crc_norm, programmed.image_crc_norm);
    ASSERT_TRUE(r->image_crc_raw != programmed.image_crc_raw);
}

TEST(verify_rejects_an_unfinished_calibration)
{
    program_chip();
    calib_site_t s = {.addr = SITE, .kind = CALIB_KIND_IHRC, .reg0 = chip->reg_ihrcr};
    calib_loop_words(chip->code_bits, &s, CALIB_LOOP_IMM, fake_chip_mem() + SITE);
    const op_result_t *r = tops_run(PDK_OP_VERIFY, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_VERIFY_FAILED);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    ASSERT_EQ(r->fail_addr, SITE);
}

TEST(verify_compares_everything_else_exactly)
{
    program_chip();
    fake_chip_mem()[0x75] &= (uint16_t)~1u;
    const op_result_t *r = tops_run(PDK_OP_VERIFY, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_VERIFY_FAILED);
    ASSERT_EQ(r->fail_addr, 0x75);
    ASSERT_EQ(r->expected, img.w[0x75]);
}

TEST(op_commands_and_status)
{
    tp_frame_t rsp;
    tp_hello(tops_seq++);
    uint8_t start[6] = {PDK_OP_PROBE, 0, 0, 0, 0, 0};
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_START, start, sizeof start, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_OK);
    uint16_t id = le16(rsp.payload);
    ASSERT_TRUE(id != 0);
    /* tp_call polls until the queue is drained: the operation has run */
    uint8_t q[2];
    put16(q, id);
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_RESULT, q, 2, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_OK);
    ASSERT_EQ(le16(rsp.payload), id);
    ASSERT_EQ(le16(rsp.payload + 4), PDK_OK);
    ASSERT_EQ(le16(rsp.payload + 7), chip->id12);
    put16(q, 0);
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_RESULT, q, 2, &rsp), 0);
    ASSERT_EQ(le16(rsp.payload), id);
    put16(q, (uint16_t)(id + 5));
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_RESULT, q, 2, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_NOT_ALLOWED_NOW);
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_CANCEL, q, 2, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_NOT_ALLOWED_NOW);

    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_GET_STATUS, NULL, 0, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_OK);
    ASSERT_EQ(rsp.len, 24);
    ASSERT_EQ(rsp.payload[0], PDK_STATE_IDLE);
    ASSERT_EQ(le16(rsp.payload + 1), id);
    ASSERT_EQ(rsp.payload[3], PDK_OP_PROBE);
    ASSERT_EQ(rsp.payload[5], 100);
    ASSERT_EQ(rsp.payload[6], PDK_PKG_EMPTY);

    uint8_t bad[6] = {PDK_OP_PROBE, PDK_OPF_SKIP_ERASE, 0, 0, 0, 0};   /* op_flags are for PROGRAM only */
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_START, bad, sizeof bad, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_BAD_PAYLOAD);
    uint8_t unknown[6] = {0x0C, 0, 0, 0, 0, 0};   /* no such operation type */
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_OP_START, unknown, sizeof unknown, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_OP_UNSUPPORTED);
}
