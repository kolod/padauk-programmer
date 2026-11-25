/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Operation manager with PROGRAM on the fake chip engine: stages, progress, cancel, session loss, BUSY,
 * journal counters, RESULT. */
#include "pdk_test.h"

#include "fake_chip.h"
#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "test_ops.h"
#include "test_pkg.h"

static tops_image_t img;
static const chip_info_t *chip;
static tp_frame_t frames[400];
static uint16_t expect[TOPS_MAX_WORDS];

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    tops_seq = 1;
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, 0x40, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_serial(&img, chip, 0x60);
    tops_put_code(&img, chip, 0x70, 0x10);
    tops_put(&img, 0x7FF, 0x3FFD);   /* fuse */
    fake_chip_insert(chip);
}

/* EVT_PROGRESS stages in order (duplicates of the same stage collapsed). */
static int progress_stages(int n, uint8_t *stages, uint8_t *count_seen)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (frames[i].type != PDK_FT_EVT || frames[i].code != PDK_EVT_PROGRESS)
            continue;
        uint8_t st = frames[i].payload[3];
        *count_seen = frames[i].payload[5];
        if (!k || stages[k - 1] != st)
            stages[k++] = st;
    }
    return k;
}

static int count_events(int n, uint16_t code)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        c += frames[i].type == PDK_FT_EVT && frames[i].code == code;
    return c;
}

TEST(program_goes_through_every_stage_in_order)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    tp_collect(frames, 400);   /* drop the upload traffic */
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_FINAL_VERIFY);
    ASSERT_EQ(r->chip_id_found, chip->id12);
    ASSERT_EQ(r->calib_count, 1);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_OK);
    ASSERT_EQ(r->serial_valid, 1);
    ASSERT_EQ(r->fuse_written, 0x3FFD);
    ASSERT_EQ(r->warnings, 0);
    ASSERT_TRUE(r->image_crc_raw != 0 && r->image_crc_norm != 0);

    int n = tp_collect(frames, 400);
    uint8_t stages[32], count = 0;
    int k = progress_stages(n, stages, &count);
    static const uint8_t want[] = {PDK_STAGE_PKG_CHECK, PDK_STAGE_ID, PDK_STAGE_ERASE, PDK_STAGE_BLANK_CHECK,
                                   PDK_STAGE_WRITE, PDK_STAGE_VERIFY, PDK_STAGE_CALIBRATE, PDK_STAGE_PKG_CHECK,
                                   PDK_STAGE_FUSE, PDK_STAGE_FINAL_VERIFY};
    ASSERT_EQ(k, (int)sizeof want);
    ASSERT_MEM_EQ(stages, want, sizeof want);
    ASSERT_EQ(count, sizeof want);
    ASSERT_EQ(count_events(n, PDK_EVT_OP_STARTED), 1);
    ASSERT_EQ(count_events(n, PDK_EVT_OP_DONE), 1);

    tops_expected(chip, &img, r, expect);
    ASSERT_MEM_EQ(fake_chip_mem(), expect, 2u * chip->code_words);
    ASSERT_EQ(journal_get()->ok_count, 1);
    ASSERT_EQ(journal_get()->fail_count, 0);
    ASSERT_TRUE(op_manager_result(0) == r);
    ASSERT_TRUE(!op_manager_busy());
}

TEST(skipped_stages_are_not_announced)
{
    img.def[0x7FF] = 0;   /* no fuse */
    img.flags = 1u << 2 | 1u << 3;   /* SKIP_ERASE | SKIP_BLANK_CHECK (FLASH chip) */
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    tp_collect(frames, 400);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, PDK_OPF_SKIP_VERIFY, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->fuse_written, 0xFFFF);
    int n = tp_collect(frames, 400);
    uint8_t stages[32], count = 0;
    int k = progress_stages(n, stages, &count);
    static const uint8_t want[] = {PDK_STAGE_PKG_CHECK, PDK_STAGE_ID, PDK_STAGE_WRITE, PDK_STAGE_CALIBRATE,
                                   PDK_STAGE_PKG_CHECK, PDK_STAGE_FINAL_VERIFY};
    ASSERT_EQ(k, (int)sizeof want);
    ASSERT_MEM_EQ(stages, want, sizeof want);
    ASSERT_EQ(fake_chip_erases(), 0);
}

static void slow_chip(void) { fake_advance_us(150000u); }

TEST(progress_comes_at_least_every_500_ms)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    tp_collect(frames, 400);
    fake_chip_hook(slow_chip);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    int n = tp_collect(frames, 400);
    ASSERT_TRUE(r->elapsed_ms > 3000);
    ASSERT_TRUE(count_events(n, PDK_EVT_PROGRESS) >= (int)(r->elapsed_ms / OP_PROGRESS_INTERVAL_MS));
}

static uint16_t running_id;

static void cancel_in_write(void)
{
    if (op_current()->stage == PDK_STAGE_WRITE)
        op_manager_cancel(running_id);
}

TEST(cancel_stops_safely)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_hook(cancel_in_write);
    op_request_t req = {.op_type = PDK_OP_PROGRAM};
    ASSERT_EQ(op_manager_start(&req, &running_id), PDK_OK);
    ASSERT_TRUE(op_manager_busy());
    ASSERT_TRUE(op_manager_result(running_id) == NULL);
    uint32_t safe_before = fake_chip_safe_states();
    pdk_core_poll();
    const op_result_t *r = op_manager_result(running_id);
    ASSERT_EQ(r->status, PDK_CANCELLED);
    ASSERT_EQ(r->stage, PDK_STAGE_WRITE);
    ASSERT_TRUE(fake_chip_safe_states() > safe_before);
    ASSERT_EQ(r->fuse_written, 0xFFFF);
    ASSERT_EQ(journal_get()->fail_count, 1);
    ASSERT_TRUE(!op_manager_cancel(running_id));   /* nothing running any more */
}

static void unplug_usb(void)
{
    if (op_current()->stage == PDK_STAGE_VERIFY)
        fake_set_open(false);
}

TEST(losing_the_session_does_not_stop_the_operation)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    fake_chip_hook(unplug_usb);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_FINAL_VERIFY);
}

static int hook_calls;
static uint16_t nested_start;

static void commands_during_operation(void)
{
    if (hook_calls++ != 3)
        return;
    uint8_t begin[128] = {0};
    tp_send(500, PDK_CMD_PKG_BEGIN, begin, sizeof begin);   /* not allowed: BUSY */
    tp_send(501, PDK_CMD_PKG_INFO, NULL, 0);                /* allowed during an operation */
    op_request_t req = {.op_type = PDK_OP_PROGRAM};
    uint16_t id;
    nested_start = op_manager_start(&req, &id);
}

TEST(busy_and_allowed_commands_during_an_operation)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    tp_collect(frames, 400);
    hook_calls = 0;
    fake_chip_hook(commands_during_operation);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(nested_start, PDK_BUSY);
    int n = tp_collect(frames, 400);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (frames[i].type != PDK_FT_RSP)
            continue;
        if (frames[i].seq == 500) {
            ASSERT_EQ(frames[i].status, PDK_BUSY);
            seen++;
        }
        if (frames[i].seq == 501) {
            ASSERT_EQ(frames[i].status, PDK_OK);
            seen++;
        }
    }
    ASSERT_EQ(seen, 2);
}

TEST(operations_wait_for_an_upload_to_finish)
{
    static uint8_t pkg[TPK_MAX_SIZE];
    tpk_build(pkg, chip, img.w, img.def, img.n, 0, 0);
    tp_frame_t rsp;
    tp_hello(tops_seq++);
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_PKG_BEGIN, pkg, 128, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_OK);
    op_request_t req = {.op_type = PDK_OP_PROGRAM};
    uint16_t id;
    ASSERT_EQ(op_manager_start(&req, &id), PDK_BUSY);
    req.op_type = 0x7F;
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_PKG_ABORT, NULL, 0, &rsp), 0);
    ASSERT_EQ(op_manager_start(&req, &id), PDK_OP_UNSUPPORTED);
}

TEST(result_encoding_has_the_contract_layout)
{
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    tp_collect(frames, 400);
    const op_result_t *r = tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL);
    int n = tp_collect(frames, 400);
    const tp_frame_t *done = NULL;
    for (int i = 0; i < n; i++)
        if (frames[i].type == PDK_FT_EVT && frames[i].code == PDK_EVT_OP_DONE)
            done = &frames[i];
    ASSERT_TRUE(done != NULL);
    const uint8_t *p = done->payload;
    ASSERT_EQ(le16(p + 0), r->op_id);
    ASSERT_EQ(p[2], PDK_OP_PROGRAM);
    ASSERT_EQ(p[3], PDK_SOURCE_HOST);
    ASSERT_EQ(le16(p + 4), PDK_OK);
    ASSERT_EQ(p[6], PDK_STAGE_FINAL_VERIFY);
    ASSERT_EQ(le16(p + 7), chip->id12);
    ASSERT_EQ(p[9], chip->family);
    ASSERT_EQ(le16(p + 10), 0xFFFF);   /* fail_addr */
    ASSERT_EQ(p[16], 1);               /* serial_valid */
    ASSERT_MEM_EQ(p + 17, r->serial, 8);
    ASSERT_EQ(le16(p + 25), 0x3FFD);   /* fuse_written */
    ASSERT_EQ(p[39], 1);               /* calib_count */
    ASSERT_EQ(p[40], CALIB_KIND_IHRC);
    ASSERT_EQ(le32(p + 42), 8000000u);
    const uint8_t *q = p + 40 + 13;
    ASSERT_EQ(le32(q + 0), r->image_crc_raw);
    ASSERT_EQ(le32(q + 4), r->image_crc_norm);
    ASSERT_EQ(q[8], 0);       /* warnings */
    ASSERT_EQ(q[9], 0xFF);    /* v_rail */
    ASSERT_EQ(q[14], 0);      /* level_count */
    ASSERT_EQ(done->len, (uint16_t)(q + 15 - p));
}
