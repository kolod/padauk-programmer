/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Serial number generator: BLAKE2s, 64 random bits per chip, UID mixing, source health checks. */
#include "pdk_test.h"

#include "fake_hal.h"
#include "package/serial_rng.h"
#include "proto/proto_defs.h"

void pdk_test_setup(void) { fake_reset(); }

TEST(blake2s_matches_rfc_7693)
{
    static const uint8_t abc[32] = {
        0x50, 0x8C, 0x5E, 0x8C, 0x32, 0x7C, 0x14, 0xE2, 0xE1, 0xA7, 0x2B, 0xA3, 0x4E, 0xEB, 0x45, 0x2F,
        0x37, 0x45, 0x8B, 0x20, 0x9E, 0xD6, 0x3A, 0x29, 0x4D, 0x99, 0x9B, 0x4C, 0x86, 0x67, 0x59, 0x82};
    static const uint8_t empty[32] = {
        0x69, 0x21, 0x7A, 0x30, 0x79, 0x90, 0x80, 0x94, 0xE1, 0x11, 0x21, 0xD0, 0x42, 0x35, 0x4A, 0x7C,
        0x1F, 0x55, 0xB6, 0x48, 0x2C, 0xA1, 0xA5, 0x1E, 0x1B, 0x25, 0x0D, 0xFD, 0x1E, 0xD0, 0xEE, 0xF9};
    uint8_t out[32];
    blake2s_256("abc", 3, out);
    ASSERT_MEM_EQ(out, abc, 32);
    blake2s_256("", 0, out);
    ASSERT_MEM_EQ(out, empty, 32);
    uint8_t long_in[200];   /* more than two 64-byte blocks: compression of full blocks */
    for (unsigned i = 0; i < sizeof long_in; i++)
        long_in[i] = (uint8_t)i;
    uint8_t a[32], b[32];
    blake2s_256(long_in, sizeof long_in, a);
    long_in[130] ^= 1;
    blake2s_256(long_in, sizeof long_in, b);
    ASSERT_TRUE(memcmp(a, b, 32) != 0);
}

TEST(consecutive_chips_get_different_numbers)
{
    uint8_t s[16][8];
    for (int i = 0; i < 16; i++)
        ASSERT_EQ(serial_rng_next(100, s[i]), PDK_OK);
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            ASSERT_TRUE(memcmp(s[i], s[j], 8) != 0);
    int ones = 0;   /* all 64 bits are used */
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 64; b++)
            ones += (s[i][b / 8] >> (b % 8)) & 1;
    ASSERT_TRUE(ones > 16 * 64 / 2 - 100 && ones < 16 * 64 / 2 + 100);
}

TEST(same_noise_on_another_programmer_gives_another_number)
{
    uint8_t a[8], b[8];
    fake_entropy(FAKE_ENTROPY_NOISE, 42);
    ASSERT_EQ(serial_rng_next(1, a), PDK_OK);
    static const uint8_t other[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    fake_uid(other);
    fake_entropy(FAKE_ENTROPY_NOISE, 42);
    ASSERT_EQ(serial_rng_next(1, b), PDK_OK);
    ASSERT_TRUE(memcmp(a, b, 8) != 0);
}

TEST(broken_source_gives_no_number)
{
    static const fake_entropy_mode_t broken[] = {FAKE_ENTROPY_CONSTANT, FAKE_ENTROPY_PERIODIC,
                                                 FAKE_ENTROPY_BIASED, FAKE_ENTROPY_FAIL};
    for (unsigned i = 0; i < sizeof broken / sizeof broken[0]; i++) {
        uint8_t s[8] = {0};
        fake_entropy(broken[i], 0);
        ASSERT_EQ(serial_rng_next(1, s), PDK_INTERNAL);
    }
    fake_entropy(FAKE_ENTROPY_NOISE, 0);
    uint8_t s[8];
    ASSERT_EQ(serial_rng_next(1, s), PDK_OK);
}

TEST(health_checks)
{
    uint8_t raw[SERIAL_RAW_BYTES];
    for (unsigned i = 0; i < sizeof raw; i++)
        raw[i] = (uint8_t)(i * 167u + 13u);   /* all different */
    ASSERT_TRUE(serial_rng_health(raw, sizeof raw));
    uint8_t r2[SERIAL_RAW_BYTES];
    memcpy(r2, raw, sizeof r2);
    memset(r2 + 10, 0x77, 16);   /* 16 identical in a row */
    ASSERT_TRUE(!serial_rng_health(r2, sizeof r2));
    memcpy(r2, raw, sizeof r2);
    for (unsigned i = 0; i < sizeof r2; i++)   /* period 8 */
        r2[i] = raw[i % 8];
    ASSERT_TRUE(!serial_rng_health(r2, sizeof r2));
    memcpy(r2, raw, sizeof r2);
    for (unsigned i = 0; i < 41; i++)   /* one value in more than 40 of 64, never 16 in a row */
        r2[(i * 3) % 64] = 0x00;
    ASSERT_TRUE(!serial_rng_health(r2, sizeof r2));
}
