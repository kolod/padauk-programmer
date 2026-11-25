/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_pkg.h"

#include <string.h>

#include "proto/crc32.h"
#include "proto/le.h"

size_t tpk_build(uint8_t *out, const chip_info_t *c, const uint16_t *w, const uint8_t *def, uint16_t n,
                 uint32_t flags, uint16_t fuse)
{
    size_t mask_bytes = 2u * ((n + 15u) / 16u);
    size_t size = 128u + 2u * n + mask_bytes + 4u;
    memset(out, 0, size);
    memcpy(out, "PDKP", 4);
    put16(out + 4, 1);
    put16(out + 6, 128);
    put32(out + 8, (uint32_t)size);
    put16(out + 12, c->id12);
    put16(out + 14, n);
    put32(out + 16, flags);
    put16(out + 20, fuse);
    size_t name_len = strlen(c->name);
    memcpy(out + 48, c->name, name_len < 16u ? name_len : 16u);
    memcpy(out + 64, "test", 4);
    put32(out + 124, crc32_compute(out, 124));
    for (uint16_t i = 0; i < n; i++) {
        put16(out + 128 + 2u * i, def[i] ? w[i] : 0xFFFF);
        if (def[i])
            out[128 + 2u * n + i / 8u] |= (uint8_t)(1u << (i % 8u));
    }
    put32(out + size - 4, crc32_compute(out, size - 4));
    return size;
}
