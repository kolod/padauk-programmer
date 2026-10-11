/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_test.h"

#include "fake_hal.h"
#include "proto/cobs.h"
#include "proto/crc32.h"
#include "vectors_frames.h"

void pdk_test_setup(void) { fake_reset(); }

TEST(crc_check_value) { ASSERT_EQ(crc32_compute(vec_crc0_data, vec_crc0_data_len), vec_crc0_value); }
TEST(crc_empty) { ASSERT_EQ(crc32_compute(vec_crc1_data, vec_crc1_data_len), vec_crc1_value); }
TEST(crc_1k) { ASSERT_EQ(crc32_compute(vec_crc2_data, vec_crc2_data_len), vec_crc2_value); }

TEST(crc_incremental_equals_oneshot)
{
    uint32_t s = crc32_init();
    s = crc32_update(s, vec_crc2_data, 100);
    s = crc32_update(s, vec_crc2_data + 100, vec_crc2_data_len - 100);
    ASSERT_EQ(crc32_final(s), vec_crc2_value);
}

static void check_cobs(const uint8_t *raw, size_t raw_len, const uint8_t *enc, size_t enc_len)
{
    uint8_t out[2000];
    size_t n = cobs_encode(raw, raw_len, out, sizeof out);
    ASSERT_EQ(n, enc_len);
    ASSERT_MEM_EQ(out, enc, enc_len);
    uint8_t dec[2000];
    int d = cobs_decode(enc, enc_len - 1, dec, sizeof dec);   /* without delimiter */
    ASSERT_EQ(d, (long long)raw_len);
    if (raw_len)
        ASSERT_MEM_EQ(dec, raw, raw_len);
}

#define COBS_CASE(i) TEST(cobs_vector_##i) { check_cobs(vec_cobs##i##_raw, vec_cobs##i##_raw_len, vec_cobs##i##_enc, vec_cobs##i##_enc_len); }
COBS_CASE(0)
COBS_CASE(1)
COBS_CASE(2)
COBS_CASE(3)
COBS_CASE(4)
COBS_CASE(5)
COBS_CASE(6)

TEST(cobs_encode_too_small)
{
    uint8_t out[3];
    uint8_t in[4] = {1, 2, 3, 4};
    ASSERT_EQ(cobs_encode(in, 4, out, sizeof out), 0);
}

TEST(cobs_decode_rejects_embedded_zero)
{
    uint8_t bad[] = {0x03, 0x11, 0x00, 0x22};
    uint8_t out[10];
    ASSERT_EQ(cobs_decode(bad, sizeof bad, out, sizeof out), -1);
}

TEST(cobs_stream_resync_after_garbage)
{
    uint8_t raw[64];
    cobs_rx_t rx;
    cobs_rx_init(&rx, raw, sizeof raw);
    uint8_t out[64];
    size_t len = 0;
    /* garbage block "05 01" (claims 4 data bytes, has 1) then delimiter → error */
    ASSERT_EQ(cobs_rx_byte(&rx, 0x05, out, sizeof out, &len), COBS_RX_NONE);
    ASSERT_EQ(cobs_rx_byte(&rx, 0x01, out, sizeof out, &len), COBS_RX_NONE);
    ASSERT_EQ(cobs_rx_byte(&rx, 0x00, out, sizeof out, &len), COBS_RX_ERROR);
    /* valid frame afterwards */
    for (size_t i = 0; i < vec_cobs2_enc_len; i++) {
        cobs_rx_result_t r = cobs_rx_byte(&rx, vec_cobs2_enc[i], out, sizeof out, &len);
        if (i + 1 < vec_cobs2_enc_len)
            ASSERT_EQ(r, COBS_RX_NONE);
        else
            ASSERT_EQ(r, COBS_RX_FRAME);
    }
    ASSERT_EQ(len, vec_cobs2_raw_len);
    ASSERT_MEM_EQ(out, vec_cobs2_raw, len);
}

TEST(cobs_stream_overflow_discards_until_delimiter)
{
    uint8_t raw[4];
    cobs_rx_t rx;
    cobs_rx_init(&rx, raw, sizeof raw);
    uint8_t out[8];
    size_t len;
    for (int i = 0; i < 10; i++)
        cobs_rx_byte(&rx, 0x01, out, sizeof out, &len);
    ASSERT_EQ(cobs_rx_byte(&rx, 0x00, out, sizeof out, &len), COBS_RX_ERROR);
    ASSERT_EQ(cobs_rx_byte(&rx, 0x00, out, sizeof out, &len), COBS_RX_NONE);   /* empty block ignored */
}
