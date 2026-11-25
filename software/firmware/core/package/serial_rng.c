/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "serial_rng.h"

#include <string.h>

#include "hal_entropy.h"
#include "proto/le.h"
#include "proto/proto_defs.h"

/* ---------------------------------------------------------------- BLAKE2s-256 (RFC 7693) */

static const uint32_t blake2s_iv[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                       0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

#define G(a, b, c, d, x, y)                     \
    do {                                        \
        v[a] = v[a] + v[b] + (x);               \
        v[d] = rotr(v[d] ^ v[a], 16);           \
        v[c] = v[c] + v[d];                     \
        v[b] = rotr(v[b] ^ v[c], 12);           \
        v[a] = v[a] + v[b] + (y);               \
        v[d] = rotr(v[d] ^ v[a], 8);            \
        v[c] = v[c] + v[d];                     \
        v[b] = rotr(v[b] ^ v[c], 7);            \
    } while (0)

static void compress(uint32_t h[8], const uint8_t block[64], uint32_t t, bool last)
{
    uint32_t m[16], v[16];
    for (unsigned i = 0; i < 16; i++)
        m[i] = le32(block + 4u * i);
    for (unsigned i = 0; i < 8; i++) {
        v[i] = h[i];
        v[i + 8] = blake2s_iv[i];
    }
    v[12] ^= t;   /* message length fits 32 bits here: t[1] stays 0 */
    if (last)
        v[14] = ~v[14];
    for (unsigned r = 0; r < 10; r++) {
        const uint8_t *s = blake2s_sigma[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (unsigned i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

void blake2s_256(const void *in, size_t len, uint8_t out[32])
{
    const uint8_t *p = in;
    uint32_t h[8];
    memcpy(h, blake2s_iv, sizeof h);
    h[0] ^= 0x01010000u ^ 32u;   /* digest length 32, no key, fanout = depth = 1 */
    uint32_t t = 0;
    while (len > 64) {
        t += 64;
        compress(h, p, t, false);
        p += 64;
        len -= 64;
    }
    uint8_t block[64] = {0};
    memcpy(block, p, len);
    t += (uint32_t)len;
    compress(h, block, t, true);
    for (unsigned i = 0; i < 8; i++)
        put32(out + 4u * i, h[i]);
}

/* ---------------------------------------------------------------- source health */

#define RCT_CUTOFF 16u    /* identical samples in a row */
#define APT_CUTOFF 40u    /* occurrences of one value in SERIAL_RAW_BYTES samples */
#define MAX_PERIOD 8u

bool serial_rng_health(const uint8_t *raw, size_t len)
{
    unsigned run = 1;
    for (size_t i = 1; i < len; i++) {
        run = raw[i] == raw[i - 1] ? run + 1 : 1;
        if (run >= RCT_CUTOFF)
            return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned count = 0;
        for (size_t j = 0; j < len; j++)
            count += raw[j] == raw[i];
        if (count > APT_CUTOFF)
            return false;
    }
    for (size_t p = 1; p <= MAX_PERIOD && p < len; p++) {
        size_t i = 0;
        while (i + p < len && raw[i] == raw[i + p])
            i++;
        if (i + p == len)
            return false;
    }
    return true;
}

/* ---------------------------------------------------------------- serial numbers */

static uint32_t counter;   /* differs for every number generated since power-on */

uint16_t serial_rng_next(uint32_t rec_seq, uint8_t out[8])
{
    uint8_t buf[SERIAL_RAW_BYTES + 12u + 4u + 4u];
    if (!hal_entropy_raw(buf, SERIAL_RAW_BYTES) || !serial_rng_health(buf, SERIAL_RAW_BYTES))
        return PDK_INTERNAL;
    hal_uid(buf + SERIAL_RAW_BYTES);
    put32(buf + SERIAL_RAW_BYTES + 12u, rec_seq);
    put32(buf + SERIAL_RAW_BYTES + 16u, ++counter);
    uint8_t digest[32];
    blake2s_256(buf, sizeof buf, digest);
    memcpy(out, digest, 8);
    return PDK_OK;
}
