/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local actions without a PC (T099, FR-054, research R16): every request, the priority (SELF_TEST not
 * passed, MASTER_RESTORE_PENDING), the indication sequence and its failure classes, requests during an
 * operation, broken or missing package, session loss, local cancel. */
#include "pdk_test.h"

#include "fake_chip.h"
#include "fake_hal.h"
#include "hal_ui.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"
#include "test_ops.h"

static tops_image_t img;
static const chip_info_t *chip;

static void boot(void)
{
    pdk_core_init();
    pdk_core_poll();   /* the start-up SELF_TEST */
    tops_seq = 1;
}

void pdk_test_setup(void)
{
    fake_reset();
    boot();
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, 0x40, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_serial(&img, chip, 0x60);
}

/* A local request, then the operation it started (if any) runs. */
static const op_result_t *local(pdk_local_request_t request)
{
    const op_result_t *before = op_manager_result(0);
    uint16_t before_id = before ? before->op_id : 0;
    fake_ui_clear();
    pdk_local_start(request);
    pdk_core_poll();
    const op_result_t *r = op_manager_result(0);
    return r && r->op_id != before_id ? r : NULL;
}

static pdk_ui_state_t last_ui(pdk_ui_detail_t *d)
{
    const pdk_ui_state_t *s;
    const pdk_ui_detail_t *det;
    size_t n = fake_ui_history(&s, &det);
    if (!n)
        return (pdk_ui_state_t)0xFF;
    if (d)
        *d = det[n - 1];
    return s[n - 1];
}

TEST(start_up_self_test_makes_the_programmer_ready)
{
    ASSERT_EQ(fake_ui_last(), PDK_UI_READY);
}

TEST(local_program)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_insert(chip);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->op_type, PDK_OP_PROGRAM);
    ASSERT_EQ(r->source, PDK_SOURCE_LOCAL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_TRUE(r->serial_valid);
    const pdk_ui_state_t *s;
    const pdk_ui_detail_t *d;
    size_t n = fake_ui_history(&s, &d);
    ASSERT_TRUE(n >= 3);
    ASSERT_EQ(s[0], PDK_UI_BUSY);
    bool saw_write = false;
    for (size_t i = 0; i + 1 < n; i++) {
        ASSERT_EQ(s[i], PDK_UI_BUSY);
        saw_write |= d[i].stage == PDK_STAGE_WRITE;
    }
    ASSERT_TRUE(saw_write);
    ASSERT_EQ(s[n - 1], PDK_UI_SUCCESS);
    ASSERT_EQ(journal_get()->ok_count, 1);
}

TEST(verify_and_self_test_requests)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_insert(chip);
    ASSERT_EQ(local(PDK_LOCAL_PROGRAM)->status, PDK_OK);
    const op_result_t *r = local(PDK_LOCAL_VERIFY);
    ASSERT_EQ(r->op_type, PDK_OP_VERIFY);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(last_ui(NULL), PDK_UI_SUCCESS);
    r = local(PDK_LOCAL_SELF_TEST);
    ASSERT_EQ(r->op_type, PDK_OP_SELF_TEST);
    ASSERT_EQ(r->status, PDK_SELFTEST_CHIP_PRESENT);   /* the chip is still in the socket */
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.fail, PDK_UI_FAIL_POWER_SELFTEST);
}

TEST(self_test_not_passed_turns_any_request_into_self_test)
{
    fake_reset();
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_STUCK);
    boot();
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);   /* the start-up run */
    ASSERT_EQ(d.status, PDK_SELFTEST_FAILED);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->op_type, PDK_OP_SELF_TEST);
    ASSERT_EQ(r->source, PDK_SOURCE_LOCAL);
    ASSERT_EQ(fake_chip_writes(), 0);
    fake_power_mode(PDK_RAIL_VPP, FAKE_POWER_OK);   /* repaired */
    r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->op_type, PDK_OP_SELF_TEST);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(last_ui(NULL), PDK_UI_READY);
    r = local(PDK_LOCAL_PROGRAM);   /* now the requested operation (no package here) */
    ASSERT_EQ(r->op_type, PDK_OP_PROGRAM);
}

TEST(restore_pending_comes_first_and_restore_alone_is_ignored)
{
    ASSERT_TRUE(local(PDK_LOCAL_RESTORE_MASTER) == NULL);   /* nothing pending */
    ASSERT_EQ(fake_ui_last(), PDK_UI_READY);
    journal_state_t st = *journal_get();
    st.flags |= PDK_FLAG_MASTER_RESTORE_PENDING;
    ASSERT_TRUE(journal_append(&st));
    ASSERT_TRUE(local(PDK_LOCAL_PROGRAM) == NULL);   /* RESTORE_MASTER: not in this build */
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.status, PDK_OP_UNSUPPORTED);
}

TEST(local_copy_runs_copy)
{
    const op_result_t *r = local(PDK_LOCAL_COPY);   /* empty socket */
    ASSERT_EQ(r->op_type, PDK_OP_COPY);
    ASSERT_EQ(r->source, PDK_SOURCE_LOCAL);
    ASSERT_EQ(r->status, PDK_CHIP_NOT_FOUND);
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.fail, PDK_UI_FAIL_CHIP);
}

TEST(missing_or_broken_package_gets_no_voltage)
{
    fake_chip_insert(chip);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->status, PDK_STORE_EMPTY);
    ASSERT_EQ(r->stage, PDK_STAGE_PKG_CHECK);
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.fail, PDK_UI_FAIL_NO_PACKAGE);
    ASSERT_EQ(journal_get()->fail_count, 0);   /* no chip was touched */

    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    uint32_t base = package_store_active_slot() == 0 ? 0x0800B000u : 0x0800D000u;
    *fake_flash_mem(base + 200) ^= 0x01;
    r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->status, PDK_STORE_CORRUPTED);
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.fail, PDK_UI_FAIL_STORAGE);
    ASSERT_EQ(fake_chip_writes(), 0);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VPP), 0);
}

static int hook_calls;

static void during_operation(void)
{
    hook_calls++;
    if (op_current()->stage == PDK_STAGE_VERIFY)
        fake_set_open(false);              /* the PC goes away: a local operation goes on */
    pdk_local_start(PDK_LOCAL_SELF_TEST);  /* ignored while an operation runs */
}

TEST(requests_during_an_operation_and_session_loss)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_insert(chip);
    fake_chip_hook(during_operation);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_TRUE(hook_calls > 0);
    ASSERT_EQ(r->op_type, PDK_OP_PROGRAM);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_TRUE(!op_manager_busy());   /* nothing was queued behind it */
}

static void cancel_in_write(void)
{
    if (op_current()->stage == PDK_STAGE_WRITE)
        pdk_local_cancel();
}

TEST(local_cancel_stops_local_operations_only)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_insert(chip);
    fake_chip_hook(cancel_in_write);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->status, PDK_CANCELLED);
    ASSERT_EQ(r->stage, PDK_STAGE_WRITE);
    r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);   /* started from the PC: not cancelled */
    ASSERT_EQ(r->status, PDK_OK);
}

TEST(finalize_on_fail_is_ignored_locally)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_insert(chip);
    fake_chip_calib(FAKE_CAL_FAIL, 0x40);
    const op_result_t *r = local(PDK_LOCAL_PROGRAM);
    ASSERT_EQ(r->status, PDK_CALIB_FAILED);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_FAILED);
    pdk_ui_detail_t d;
    ASSERT_EQ(last_ui(&d), PDK_UI_FAIL);
    ASSERT_EQ(d.fail, PDK_UI_FAIL_CALIB);
}
