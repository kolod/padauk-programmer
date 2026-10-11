/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Header and commit checks against the shared package vectors (tests/vectors/packages/manifest.json). */
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

static int expected_stage(const pkg_vector_t *v)
{
    switch (v->stage) {
    case PV_BEGIN: return TP_STAGE_BEGIN;
    case PV_COMMIT: return TP_STAGE_COMMIT;
    default: return TP_STAGE_OK;
    }
}

TEST(every_vector_gives_exact_status_and_addr)
{
    for (size_t i = 0; i < PKG_VECTOR_COUNT; i++) {
        const pkg_vector_t *v = &pkg_vectors[i];
        int stage = tp_upload(v->data, v->len, &seq, 508, &r);
        if (stage != expected_stage(v))
            PDK_FAIL("%s: stage %d, expected %d (status 0x%04x)", v->name, stage, expected_stage(v), r.status);
        if (r.status != v->status)
            PDK_FAIL("%s: status 0x%04x, expected 0x%04x", v->name, r.status, v->status);
        if (v->stage == PV_COMMIT) {
            if (r.len != 2 || le16(r.payload) != v->addr)
                PDK_FAIL("%s: addr 0x%04x, expected 0x%04x", v->name, r.len == 2 ? le16(r.payload) : 0, v->addr);
        }
        if (v->stage == PV_OK) {
            if (r.len != 8)
                PDK_FAIL("%s: commit result length %u", v->name, r.len);
            if (le32(r.payload) != le32(v->data + v->len - 4))
                PDK_FAIL("%s: crc mismatch", v->name);
            if (r.payload[4] != v->calib_sites || r.payload[5] != v->serial_sites ||
                le16(r.payload + 6) != v->effective_words)
                PDK_FAIL("%s: calib %u serial %u eff %u, expected %u %u %u", v->name, r.payload[4], r.payload[5],
                         le16(r.payload + 6), v->calib_sites, v->serial_sites, v->effective_words);
        }
    }
}

TEST(rejected_package_leaves_active_one)
{
    const pkg_vector_t *good = &pkg_vectors[0];
    ASSERT_EQ(tp_upload(good->data, good->len, &seq, 508, &r), TP_STAGE_OK);
    for (size_t i = 0; i < PKG_VECTOR_COUNT; i++) {
        const pkg_vector_t *v = &pkg_vectors[i];
        if (v->stage == PV_OK)
            continue;
        tp_upload(v->data, v->len, &seq, 508, &r);
        ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_INFO, NULL, 0, &r), 0);
        ASSERT_EQ(r.status, PDK_OK);
        if (r.payload[0] != PDK_PKG_VALID || le32(r.payload + 1) != le32(good->data + good->len - 4))
            PDK_FAIL("%s replaced the active package", v->name);
        ASSERT_MEM_EQ(r.payload + 5, good->data, 128);
    }
}

TEST(package_survives_restart)
{
    const pkg_vector_t *v = &pkg_vectors[1];
    ASSERT_EQ(tp_upload(v->data, v->len, &seq, 508, &r), TP_STAGE_OK);
    pdk_core_init();   /* "power cycle": flash keeps its content */
    tp_hello(seq++);
    ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_INFO, NULL, 0, &r), 0);
    ASSERT_EQ(r.payload[0], PDK_PKG_VALID);
    ASSERT_EQ(le32(r.payload + 1), le32(v->data + v->len - 4));
    ASSERT_MEM_EQ(r.payload + 5, v->data, 128);

    uint8_t req[6];
    uint8_t back[4096];
    for (uint32_t off = 0; off < v->len;) {
        uint16_t n = (uint16_t)(v->len - off < 256 ? v->len - off : 256);
        put32(req, off);
        put16(req + 4, n);
        ASSERT_EQ(tp_call(seq++, PDK_CMD_PKG_READ, req, 6, &r), 0);
        ASSERT_EQ(r.status, PDK_OK);
        ASSERT_EQ(r.len, n);
        memcpy(back + off, r.payload, n);
        off += n;
    }
    ASSERT_MEM_EQ(back, v->data, v->len);
}
