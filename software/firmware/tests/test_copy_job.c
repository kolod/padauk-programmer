/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CopyJob (T106, without MEASURE_MASTER — part 7c): a measurement loop in the master, permissions for
 * modifying the master, MASTER_RESTORE_PENDING blocking COPY / PKG_BEGIN / PKG_DELETE. */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "ops/op_pkg.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"
#include "test_copy_helpers.h"
#include "test_ops.h"

#define IHRC 0x40u

static tops_image_t img;
static const chip_info_t *chip;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    pdk_core_poll();
    tops_seq = 1;
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, IHRC, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put(&img, 0x7FF, 0x3FFD);
}

/* A master whose calibration never finished: the loop is still there. */
static void uncalibrated_master(void)
{
    fake_chip_insert(chip);
    fake_chip_calib(FAKE_CAL_FAIL, 0x40);
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    ASSERT_EQ(tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL)->status, PDK_CALIB_FAILED);
}

TEST(master_loop_needs_permission_and_a_target)
{
    uncalibrated_master();
    uint32_t crc = package_store_crc();
    const op_result_t *r = tops_copy(chip, NULL);
    ASSERT_EQ(r->status, PDK_COPY_MASTER_UNCALIBRATED);
    ASSERT_EQ(r->stage, PDK_STAGE_READ);
    ASSERT_EQ(r->fail_addr, IHRC);
    ASSERT_EQ(r->calib_count, 1);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_MASTER_LOOP);
    ASSERT_EQ(package_store_crc(), crc);   /* nothing stored */

    copy_ext_t allow_keep = {.flags = 2};   /* ALLOW_MASTER_LOOP, IHRC still KEEP_VALUE */
    ASSERT_EQ(tops_copy(chip, &allow_keep)->status, PDK_BAD_PAYLOAD);
    ASSERT_EQ(package_store_crc(), crc);

    copy_ext_t allow = {.flags = 2, .mode_ihrc = 1, .target_ihrc = 4000000};
    r = tops_copy(chip, &allow);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(op_pkg_load(), PDK_OK);
    ASSERT_EQ(op_pkg.plan.calib_count, 1);
    ASSERT_EQ(op_pkg.plan.calib[0].target_hz, 4000000u);   /* FR-051: the target from the PC */
}

TEST(modifying_the_master_needs_permission)
{
    fake_chip_insert(chip);
    copy_ext_t measure = {.mode_ihrc = 3};
    ASSERT_EQ(tops_copy(chip, &measure)->status, PDK_MASTER_MODIFY_NOT_ALLOWED);
    const chip_info_t *otp = tops_chip("PMS150C");
    fake_chip_insert(otp);
    measure.flags = 1;   /* ALLOW_MODIFY_MASTER */
    ASSERT_EQ(tops_copy(otp, &measure)->status, PDK_MASTER_NOT_REWRITABLE);
}

TEST(restore_pending_blocks_copy_and_the_package_store)
{
    fake_chip_insert(chip);
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    journal_state_t st = *journal_get();
    st.flags |= PDK_FLAG_MASTER_RESTORE_PENDING;
    ASSERT_TRUE(journal_append(&st));
    ASSERT_EQ(tops_copy(chip, NULL)->status, PDK_MASTER_RESTORE_PENDING);
    tp_frame_t rsp;
    uint8_t hdr[128] = {0};
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_PKG_BEGIN, hdr, sizeof hdr, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_MASTER_RESTORE_PENDING);
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_PKG_DELETE, NULL, 0, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_MASTER_RESTORE_PENDING);
}
