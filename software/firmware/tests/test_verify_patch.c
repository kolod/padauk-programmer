/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PATCH on the fake chip engine (T069; VERIFY with placeholders is in test_op_simple.c): checksum modes,
 * the 1→0 rule, nothing written before both checks pass, reserved area, fuse protection, CHECK_ONLY. */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_ops.h"

#define SITE 0x40u

enum { CHECK_ONLY = 1, ALLOW_SECURITY_FUSE = 2 };
enum { RAW = 0, NORMALIZED = 1, ACTIVE_PACKAGE = 2 };

static tops_image_t img;
static const chip_info_t *chip;
static uint16_t before[TOPS_MAX_WORDS];
static op_result_t programmed;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    tops_seq = 1;
    chip = tops_chip("PMS150C");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, SITE, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_serial(&img, chip, 0x60);
    tops_put_code(&img, chip, 0x70, 0x20);
    tops_put(&img, 0x3FF, 0x1FFD);
    fake_chip_insert(chip);
    fake_chip_calib(FAKE_CAL_OK, 0x37);
    tops_upload(chip, &img);
    programmed = *tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
}

static const op_result_t *patch(uint8_t flags, uint8_t mode, uint32_t crc, uint8_t count, const uint16_t *addr,
                                const uint16_t *value)
{
    static op_request_t req;
    memset(&req, 0, sizeof req);
    req.op_type = PDK_OP_PATCH;
    req.chip_id12 = chip->id12;
    req.ext[0] = flags;
    req.ext[1] = mode;
    put32(req.ext + 2, crc);
    req.ext[6] = count;
    for (unsigned i = 0; i < count; i++) {
        put16(req.ext + 7 + 4 * i, addr[i]);
        put16(req.ext + 9 + 4 * i, value[i]);
    }
    req.ext_len = (uint16_t)(7u + 4u * count);
    memcpy(before, fake_chip_mem(), 2u * chip->code_words);
    return tops_run_req(&req, NULL);
}

static bool chip_unchanged(void) { return !memcmp(before, fake_chip_mem(), 2u * chip->code_words); }

TEST(compatible_patch_against_the_active_package)
{
    ASSERT_EQ(programmed.status, PDK_OK);
    uint16_t a[2] = {0x10, 0x11}, v[2];
    v[0] = (uint16_t)(fake_chip_mem()[0x10] & ~1u);   /* 1 → 0 only */
    v[1] = fake_chip_mem()[0x11];                     /* already so: not written */
    uint32_t writes = fake_chip_writes();
    const op_result_t *r = patch(0, ACTIVE_PACKAGE, 0, 2, a, v);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_VERIFY);
    ASSERT_EQ(r->data_words, 1);
    ASSERT_EQ(fake_chip_writes(), writes + 1);
    ASSERT_EQ(fake_chip_mem()[0x10], v[0]);
    before[0x10] = v[0];
    ASSERT_TRUE(chip_unchanged());   /* nothing else changed */
    ASSERT_TRUE(r->image_crc_raw != programmed.image_crc_raw);
}

TEST(raw_and_normalized_checksums)
{
    uint16_t a = 0x12, v = (uint16_t)(fake_chip_mem()[0x12] & ~1u);
    const op_result_t *r = patch(0, RAW, programmed.image_crc_raw ^ 1, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_PATCH_IMAGE_MISMATCH);
    ASSERT_EQ(r->image_crc_raw, programmed.image_crc_raw);   /* both sums come back */
    ASSERT_EQ(r->image_crc_norm, programmed.image_crc_norm);
    ASSERT_TRUE(chip_unchanged());
    r = patch(0, RAW, programmed.image_crc_raw, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_OK);

    /* another chip of the batch: other k, other serial, the same normalized sum */
    fake_chip_insert(chip);
    fake_chip_calib(FAKE_CAL_OK, 0x52);
    ASSERT_EQ(tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL)->status, PDK_OK);
    v = (uint16_t)(fake_chip_mem()[0x12] & ~1u);
    r = patch(0, NORMALIZED, programmed.image_crc_norm, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_OK);
}

TEST(conflict_writes_nothing)
{
    uint16_t a[2] = {0x13, 0x14}, v[2];
    v[0] = (uint16_t)(fake_chip_mem()[0x13] & ~1u);   /* fine */
    v[1] = (uint16_t)(fake_chip_mem()[0x14] | 0x0002u);   /* needs 0 → 1 */
    uint32_t writes = fake_chip_writes();
    const op_result_t *r = patch(0, ACTIVE_PACKAGE, 0, 2, a, v);
    ASSERT_EQ(r->status, PDK_PATCH_CONFLICT);
    ASSERT_EQ(r->fail_addr, 0x14);
    ASSERT_EQ(r->expected, v[1]);
    ASSERT_EQ(r->actual, before[0x14]);
    ASSERT_EQ(fake_chip_writes(), writes);
    ASSERT_TRUE(chip_unchanged());
}

TEST(addresses_and_counts)
{
    uint16_t a = 0x3F8, v = 0;   /* reserved area */
    const op_result_t *r = patch(0, ACTIVE_PACKAGE, 0, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_PKG_RESERVED_AREA);
    ASSERT_EQ(r->fail_addr, 0x3F8);
    a = 0x400;
    ASSERT_EQ(patch(0, ACTIVE_PACKAGE, 0, 1, &a, &v)->status, PDK_BAD_PAYLOAD);
    ASSERT_EQ(patch(0, ACTIVE_PACKAGE, 0, 0, NULL, NULL)->status, PDK_BAD_PAYLOAD);
    uint16_t twice[2] = {0x20, 0x20}, vals[2] = {0, 0};
    ASSERT_EQ(patch(0, ACTIVE_PACKAGE, 0, 2, twice, vals)->status, PDK_BAD_PAYLOAD);
    ASSERT_TRUE(chip_unchanged());
    op_request_t req = {.op_type = PDK_OP_PATCH};   /* chip type is required */
    uint16_t id;
    ASSERT_EQ(op_manager_start(&req, &id), PDK_BAD_PAYLOAD);
}

TEST(protection_fuse_needs_permission)
{
    uint16_t a = 0x3FF, v = 0x1FFC;   /* bit 0 = 0: read protection */
    const op_result_t *r = patch(0, ACTIVE_PACKAGE, 0, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_PKG_SECURITY_NOT_ALLOWED);
    ASSERT_TRUE(chip_unchanged());
    r = patch(ALLOW_SECURITY_FUSE, ACTIVE_PACKAGE, 0, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(fake_chip_mem()[0x3FF], 0x1FFC);
}

TEST(check_only_writes_nothing)
{
    uint16_t a = 0x15, v = (uint16_t)(fake_chip_mem()[0x15] & ~1u);
    uint32_t writes = fake_chip_writes();
    const op_result_t *r = patch(CHECK_ONLY, ACTIVE_PACKAGE, 0, 1, &a, &v);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_READ);
    ASSERT_EQ(r->data_words, 0);
    r = patch(CHECK_ONLY, RAW, 0, 0, NULL, NULL);   /* no expected sum: just report both */
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->image_crc_raw, programmed.image_crc_raw);
    ASSERT_EQ(r->image_crc_norm, programmed.image_crc_norm);
    ASSERT_EQ(fake_chip_writes(), writes);
    ASSERT_TRUE(chip_unchanged());
}

TEST(active_package_special_cases)
{
    calib_site_t s = {.addr = SITE, .kind = CALIB_KIND_IHRC, .reg0 = chip->reg_ihrcr};
    uint16_t *m = fake_chip_mem();   /* an unfinished calibration: the normalized sums differ */
    calib_loop_words(chip->code_bits, &s, CALIB_LOOP_IMM, m + SITE);
    uint16_t a = 0x16, v = (uint16_t)(m[0x16] & ~1u);
    ASSERT_EQ(patch(0, ACTIVE_PACKAGE, 0, 1, &a, &v)->status, PDK_PATCH_IMAGE_MISMATCH);
    ASSERT_TRUE(chip_unchanged());

    tp_frame_t rsp;   /* no package */
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_PKG_DELETE, NULL, 0, &rsp), 0);
    ASSERT_EQ(patch(0, ACTIVE_PACKAGE, 0, 1, &a, &v)->status, PDK_STORE_EMPTY);
}
