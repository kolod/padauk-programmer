/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Upload state machine: PKG_BEGIN / PKG_DATA / PKG_COMMIT / PKG_ABORT (contracts/protocol.md). */
#include "pdk_test.h"

#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_proto.h"
#include "vectors_packages.h"

static tp_frame_t r;
static uint16_t seq;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    seq = 1;
    tp_hello(seq++);
}

static const pkg_vector_t *find(const char *name)
{
    for (size_t i = 0; i < PKG_VECTOR_COUNT; i++)
        if (!strcmp(pkg_vectors[i].name, name))
            return &pkg_vectors[i];
    return NULL;
}

static uint16_t data(uint32_t off, const uint8_t *p, uint16_t n)
{
    static uint8_t buf[4 + 512];
    put32(buf, off);
    memcpy(buf + 4, p, n);
    tp_call(seq++, PDK_CMD_PKG_DATA, buf, (uint16_t)(4 + n), &r);
    return r.status;
}

static uint16_t call(uint16_t code)
{
    tp_call(seq++, code, NULL, 0, &r);
    return r.status;
}

static uint32_t active_crc(void)
{
    call(PDK_CMD_PKG_INFO);
    return r.payload[0] == PDK_PKG_VALID ? le32(r.payload + 1) : 0;
}

static uint32_t trailer(const pkg_vector_t *v) { return le32(v->data + v->len - 4); }

TEST(begin_returns_expected_size)
{
    const pkg_vector_t *v = find("ok_pfs154_fuse_word");
    ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r), 0);
    ASSERT_EQ(r.status, PDK_OK);
    ASSERT_EQ(r.len, 4);
    ASSERT_EQ(le32(r.payload), v->len);
    ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 100, &r), 0);
    ASSERT_EQ(r.status, PDK_BAD_PAYLOAD);
}

TEST(sequential_data_and_commit)
{
    const pkg_vector_t *v = find("ok_pfs154_fuse_word");
    ASSERT_EQ(tp_upload(v->data, v->len, &seq, 300, &r), TP_STAGE_OK);
    ASSERT_EQ(le32(r.payload), trailer(v));
    ASSERT_EQ(active_crc(), trailer(v));
}

TEST(wrong_offset_reports_expected_and_aborts)
{
    const pkg_vector_t *v = find("ok_pfs154_fuse_word");
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    ASSERT_EQ(data(0, v->data, 256), PDK_OK);
    ASSERT_EQ(data(512, v->data + 512, 256), PDK_PKG_OFFSET);
    ASSERT_EQ(r.len, 4);
    ASSERT_EQ(le32(r.payload), 256);
    ASSERT_EQ(data(256, v->data + 256, 256), PDK_NOT_ALLOWED_NOW);   /* upload was aborted */
}

TEST(odd_length_or_overrun_is_bad_payload)
{
    const pkg_vector_t *v = find("ok_pfs154_3words");   /* 140 bytes */
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    ASSERT_EQ(data(0, v->data, 127), PDK_BAD_PAYLOAD);
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    static uint8_t big[200];
    memcpy(big, v->data, v->len);
    ASSERT_EQ(data(0, big, 142), PDK_BAD_PAYLOAD);
}

TEST(header_in_data_must_match_begin)
{
    const pkg_vector_t *a = find("ok_pfs154_3words"), *b = find("ok_pfs154_and5");
    tp_call(seq++, PDK_CMD_PKG_BEGIN, a->data, 128, &r);
    ASSERT_EQ(data(0, b->data, 128), PDK_PKG_HEADER_INVALID);
    ASSERT_EQ(call(PDK_CMD_PKG_COMMIT), PDK_NOT_ALLOWED_NOW);
}

TEST(commit_before_all_data_is_not_allowed)
{
    const pkg_vector_t *v = find("ok_pfs154_3words");
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    ASSERT_EQ(data(0, v->data, 128), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_PKG_COMMIT), PDK_NOT_ALLOWED_NOW);
}

TEST(abort_keeps_active_package)
{
    const pkg_vector_t *a = find("ok_pfs154_3words"), *b = find("ok_pfs154_and5");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
    tp_call(seq++, PDK_CMD_PKG_BEGIN, b->data, 128, &r);
    ASSERT_EQ(data(0, b->data, (uint16_t)b->len), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_PKG_ABORT), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_PKG_COMMIT), PDK_NOT_ALLOWED_NOW);
    ASSERT_EQ(active_crc(), trailer(a));
}

TEST(session_drop_cancels_upload)
{
    const pkg_vector_t *a = find("ok_pfs154_3words"), *b = find("ok_pfs154_and5");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
    tp_call(seq++, PDK_CMD_PKG_BEGIN, b->data, 128, &r);
    ASSERT_EQ(data(0, b->data, (uint16_t)b->len), PDK_OK);
    fake_set_open(false);
    for (int i = 0; i < 4; i++)
        pdk_core_poll();
    fake_set_open(true);
    tp_hello(seq++);
    ASSERT_EQ(call(PDK_CMD_PKG_COMMIT), PDK_NOT_ALLOWED_NOW);
    ASSERT_EQ(active_crc(), trailer(a));
}

/* Erases done inside commands (not by background housekeeping). */
static uint32_t foreground_erases(void) { return fake_flash_erases() - fake_flash_bg_started(); }

static void idle(int polls)
{
    while (polls--)
        pdk_core_poll();
}

TEST(upload_into_clean_slot_erases_nothing)
{
    const pkg_vector_t *v = find("ok_pfs154_fuse_word");   /* 4484 bytes: 3 pages */
    ASSERT_EQ(tp_upload(v->data, v->len, &seq, 508, &r), TP_STAGE_OK);
    ASSERT_EQ(fake_flash_erases(), 0);
}

TEST(back_to_back_uploads_never_erase_inside_a_command)
{
    const pkg_vector_t *a = find("ok_pfs154_fuse_word"), *b = find("ok_pfs154_ihrc");
    for (int i = 0; i < 6; i++) {
        const pkg_vector_t *v = (i & 1) ? b : a;
        ASSERT_EQ(tp_upload(v->data, v->len, &seq, 508, &r), TP_STAGE_OK);
        ASSERT_EQ(active_crc(), trailer(v));
    }
    ASSERT_EQ(foreground_erases(), 0);
    ASSERT_TRUE(fake_flash_bg_started() > 0);   /* the previous package was wiped in the background */
    ASSERT_TRUE(!fake_flash_bg_violation());
}

TEST(dirty_slot_is_erased_lazily_one_page_per_data)
{
    for (uint32_t a = 0x0800B000u; a < 0x0800F000u; a += 0x100u)   /* garbage in both slots */
        *fake_flash_mem(a) = 0x00;
    fake_flash_bg_busy_polls(100000);   /* background erase cannot keep up */
    pdk_core_init();
    tp_hello(seq++);
    const pkg_vector_t *v = find("ok_pfs154_fuse_word");
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    for (uint32_t off = 0; off < v->len;) {
        uint16_t n = (uint16_t)(v->len - off < 508 ? v->len - off : 508);
        uint32_t before = foreground_erases();
        ASSERT_EQ(data(off, v->data + off, n), PDK_OK);
        ASSERT_TRUE(foreground_erases() - before <= 1);
        off += n;
    }
    ASSERT_EQ(call(PDK_CMD_PKG_COMMIT), PDK_OK);
    ASSERT_EQ(active_crc(), trailer(v));
    ASSERT_TRUE(!fake_flash_bg_violation());
}

TEST(upload_waits_for_background_erase_of_its_page)
{
    const pkg_vector_t *a = find("ok_pfs154_fuse_word"), *b = find("ok_pfs154_ihrc");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);   /* slot 0 */
    ASSERT_EQ(tp_upload(b->data, b->len, &seq, 508, &r), TP_STAGE_OK);   /* slot 1, slot 0 now dirty */
    fake_flash_bg_busy_polls(1000);
    pdk_core_poll();   /* a background erase of slot 0 starts and stays busy */
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);   /* targets slot 0 */
    ASSERT_EQ(active_crc(), trailer(a));
    ASSERT_TRUE(!fake_flash_bg_violation());
}

TEST(deleted_package_is_wiped_in_the_background)
{
    const pkg_vector_t *a = find("ok_pfs154_fuse_word");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
    ASSERT_EQ(call(PDK_CMD_PKG_DELETE), PDK_OK);
    idle(20);
    for (uint32_t addr = 0x0800B000u; addr < 0x0800F000u; addr++)
        if (*fake_flash_mem(addr) != 0xFF)
            PDK_FAIL("byte at 0x%08x survived delete", addr);
}

TEST(aborted_upload_is_wiped_in_the_background)
{
    const pkg_vector_t *a = find("ok_pfs154_3words"), *b = find("ok_pfs154_fuse_word");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
    tp_call(seq++, PDK_CMD_PKG_BEGIN, b->data, 128, &r);
    ASSERT_EQ(data(0, b->data, 508), PDK_OK);
    ASSERT_EQ(call(PDK_CMD_PKG_ABORT), PDK_OK);
    idle(20);
    for (uint32_t addr = 0x0800D000u; addr < 0x0800F000u; addr++)   /* the upload went to slot 1 */
        if (*fake_flash_mem(addr) != 0xFF)
            PDK_FAIL("byte at 0x%08x survived abort", addr);
    ASSERT_EQ(active_crc(), trailer(a));   /* the active slot is never touched */
}

TEST(duplicate_seq_does_not_duplicate_data)
{
    const pkg_vector_t *v = find("ok_pfs154_3words");
    tp_call(seq++, PDK_CMD_PKG_BEGIN, v->data, 128, &r);
    static uint8_t buf[4 + 140];
    put32(buf, 0);
    memcpy(buf + 4, v->data, 128);
    uint16_t s = seq++;
    tp_call(s, PDK_CMD_PKG_DATA, buf, 4 + 128, &r);
    ASSERT_EQ(r.status, PDK_OK);
    tp_call(s, PDK_CMD_PKG_DATA, buf, 4 + 128, &r);   /* retransmission */
    ASSERT_EQ(r.status, PDK_OK);
    ASSERT_EQ(le32(r.payload), 128);
    ASSERT_EQ(data(128, v->data + 128, 12), PDK_OK);
    ASSERT_EQ(le32(r.payload), 140);
    s = seq++;
    tp_call(s, PDK_CMD_PKG_COMMIT, NULL, 0, &r);
    ASSERT_EQ(r.status, PDK_OK);
    uint32_t ops = fake_flash_ops();
    tp_call(s, PDK_CMD_PKG_COMMIT, NULL, 0, &r);
    ASSERT_EQ(r.status, PDK_OK);
    ASSERT_EQ(fake_flash_ops(), ops);   /* no second journal record */
}

/* Power loss at every flash operation of an upload + commit: after restart the active package is
 * always valid and is either the old or the new one (SC-007 on the core). */
TEST(power_loss_at_every_operation_is_atomic)
{
    const pkg_vector_t *a = find("ok_pfs154_3words"), *b = find("ok_pfs154_fuse_word");
    for (uint32_t n = 0;; n++) {
        pdk_test_setup();
        ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
        fake_flash_fail_after(n);
        int stage = tp_upload(b->data, b->len, &seq, 508, &r);
        bool failed = fake_flash_failed();
        fake_flash_power_restored();
        pdk_core_init();
        tp_hello(seq++);
        uint32_t crc = active_crc();
        if (crc != trailer(a) && crc != trailer(b))
            PDK_FAIL("power loss after %u ops: active crc 0x%08x", n, crc);
        if (!failed) {
            ASSERT_EQ(stage, TP_STAGE_OK);
            ASSERT_EQ(crc, trailer(b));
            ASSERT_TRUE(n > 3);
            break;
        }
    }
}

TEST(corrupted_active_package_is_reported)
{
    const pkg_vector_t *a = find("ok_pfs154_3words");
    ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
    call(PDK_CMD_PKG_INFO);
    ASSERT_EQ(r.payload[0], PDK_PKG_VALID);
    /* clear a set bit of word 0 (0x0070) in whichever slot holds it (1 → 0 is always programmable) */
    for (uint32_t base = 0x0800B000u; base <= 0x0800D000u; base += 0x2000u) {
        uint8_t *m = fake_flash_mem(base + 128);
        if (*m == 0x70)
            *m = 0x60;
    }
    pdk_core_init();
    tp_hello(seq++);
    call(PDK_CMD_PKG_INFO);
    ASSERT_EQ(r.payload[0], PDK_PKG_CORRUPTED);
    call(PDK_CMD_PKG_CHECK);
    ASSERT_EQ(r.payload[0], PDK_PKG_CORRUPTED);
}
