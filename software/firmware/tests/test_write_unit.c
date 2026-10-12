/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Uploads on flash that programs 8-byte words only (STM32G4 with ECC): PKG_DATA chunks are not
 * aligned to the write unit, the core holds back partial words and pads the last one. */
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
    fake_flash_write_unit(8);
    pdk_core_init();
    seq = 1;
    tp_hello(seq++);
}

static uint32_t trailer(const pkg_vector_t *v) { return le32(v->data + v->len - 4); }

static bool read_back_equals(const pkg_vector_t *v)
{
    uint8_t req[6];
    static uint8_t back[8192];
    for (uint32_t off = 0; off < v->len;) {
        uint16_t n = (uint16_t)(v->len - off < 500 ? v->len - off : 500);
        put32(req, off);
        put16(req + 4, n);
        if (tp_call(seq++, PDK_CMD_PKG_READ, req, 6, &r) || r.status != PDK_OK || r.len != n)
            return false;
        memcpy(back + off, r.payload, n);
        off += n;
    }
    return memcmp(back, v->data, v->len) == 0;
}

TEST(valid_vectors_with_unaligned_chunks)
{
    static const uint16_t chunks[] = {508, 300, 130, 2};
    for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
        for (size_t i = 0; i < PKG_VECTOR_COUNT; i++) {
            const pkg_vector_t *v = &pkg_vectors[i];
            if (v->stage != PV_OK || (chunks[c] == 2 && v->len > 1024))
                continue;
            if (tp_upload(v->data, v->len, &seq, chunks[c], &r) != TP_STAGE_OK)
                PDK_FAIL("%s, chunk %u: status 0x%04x", v->name, chunks[c], r.status);
            if (le32(r.payload) != trailer(v))
                PDK_FAIL("%s, chunk %u: crc", v->name, chunks[c]);
            if (!read_back_equals(v))
                PDK_FAIL("%s, chunk %u: read back differs", v->name, chunks[c]);
        }
    }
    ASSERT_TRUE(!fake_flash_bg_violation());
}

TEST(invalid_vectors_still_rejected)
{
    for (size_t i = 0; i < PKG_VECTOR_COUNT; i++) {
        const pkg_vector_t *v = &pkg_vectors[i];
        if (v->stage == PV_OK)
            continue;
        tp_upload(v->data, v->len, &seq, 508, &r);
        if (r.status != v->status)
            PDK_FAIL("%s: status 0x%04x, expected 0x%04x", v->name, r.status, v->status);
    }
}

TEST(survives_restart)
{
    const pkg_vector_t *v = &pkg_vectors[1];
    ASSERT_EQ(tp_upload(v->data, v->len, &seq, 508, &r), TP_STAGE_OK);
    pdk_core_init();
    tp_hello(seq++);
    ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_INFO, NULL, 0, &r), 0);
    ASSERT_EQ(r.payload[0], PDK_PKG_VALID);
    ASSERT_EQ(le32(r.payload + 1), trailer(v));
    ASSERT_TRUE(read_back_equals(v));
}

TEST(power_loss_at_every_operation_is_atomic)
{
    const pkg_vector_t *a = &pkg_vectors[0], *b = &pkg_vectors[5];
    for (uint32_t n = 0;; n++) {
        pdk_test_setup();
        ASSERT_EQ(tp_upload(a->data, a->len, &seq, 508, &r), TP_STAGE_OK);
        fake_flash_fail_after(n);
        int stage = tp_upload(b->data, b->len, &seq, 300, &r);
        bool failed = fake_flash_failed();
        fake_flash_power_restored();
        pdk_core_init();
        tp_hello(seq++);
        tp_call(seq++, PDK_CMD_PKG_INFO, NULL, 0, &r);
        uint32_t crc = r.payload[0] == PDK_PKG_VALID ? le32(r.payload + 1) : 0;
        if (crc != trailer(a) && crc != trailer(b))
            PDK_FAIL("power loss after %u ops: active crc 0x%08x", n, crc);
        if (!failed) {
            ASSERT_EQ(stage, TP_STAGE_OK);
            ASSERT_EQ(crc, trailer(b));
            break;
        }
    }
}
