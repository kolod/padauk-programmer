/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_test.h"

#include "fake_hal.h"
#include "pdk_config.h"
#include "proto/frame.h"
#include "proto/proto_defs.h"
#include "vectors_frames.h"

void pdk_test_setup(void) { fake_reset(); }

static void check_vector(const uint8_t *dec, size_t dec_len, const uint8_t *payload, size_t plen, const uint16_t meta[4])
{
    pdk_frame_t f;
    ASSERT_EQ(frame_parse(dec, dec_len, 1024, &f), PDK_OK);
    ASSERT_EQ(f.type, meta[0]);
    ASSERT_EQ(f.seq, meta[1]);
    ASSERT_EQ(f.code, meta[2]);
    ASSERT_EQ(f.status, meta[3]);
    ASSERT_EQ(f.len, plen);
    if (plen)
        ASSERT_MEM_EQ(f.payload, payload, plen);

    uint8_t out[1200];
    pdk_frame_t g = f;
    ASSERT_EQ(frame_encode(&g, out, sizeof out), dec_len);
    ASSERT_MEM_EQ(out, dec, dec_len);
}

#define FRAME_CASE(n) TEST(frame_##n) { check_vector(vec_frame_##n##_decoded, vec_frame_##n##_decoded_len, \
    vec_frame_##n##_payload, vec_frame_##n##_payload_len, vec_frame_##n##_meta); }
FRAME_CASE(req_hello)
FRAME_CASE(rsp_ok_empty)
FRAME_CASE(evt_progress)
FRAME_CASE(nak)
FRAME_CASE(req_zeros)
FRAME_CASE(rsp_error)

TEST(frame_rejects_bad_crc)
{
    uint8_t buf[64];
    memcpy(buf, vec_frame_req_hello_decoded, vec_frame_req_hello_decoded_len);
    buf[vec_frame_req_hello_decoded_len - 1] ^= 0x01;
    pdk_frame_t f;
    ASSERT_EQ(frame_parse(buf, vec_frame_req_hello_decoded_len, 1024, &f), PDK_FRAME_ERROR);
}

TEST(frame_rejects_length_mismatch)
{
    pdk_frame_t f;
    ASSERT_EQ(frame_parse(vec_frame_req_hello_decoded, vec_frame_req_hello_decoded_len - 1, 1024, &f), PDK_FRAME_ERROR);
    ASSERT_EQ(frame_parse(vec_frame_req_hello_decoded, 5, 1024, &f), PDK_FRAME_ERROR);
}

TEST(frame_rejects_payload_over_limit)
{
    pdk_frame_t f;
    /* req_zeros carries 300 bytes */
    ASSERT_EQ(frame_parse(vec_frame_req_zeros_decoded, vec_frame_req_zeros_decoded_len, 299, &f), PDK_FRAME_ERROR);
    ASSERT_EQ(frame_parse(vec_frame_req_zeros_decoded, vec_frame_req_zeros_decoded_len, 300, &f), PDK_OK);
}

TEST(frame_encode_too_small)
{
    uint8_t out[10];
    pdk_frame_t f = {.type = PDK_FT_REQ, .len = 0};
    ASSERT_EQ(frame_encode(&f, out, sizeof out), 0);
}
