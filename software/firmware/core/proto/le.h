/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Little-endian field helpers for protocol payloads. */
#ifndef PDK_LE_H
#define PDK_LE_H

#include <stdint.h>
#include <string.h>

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Copies a NUL-padded string field of fixed size. */
static inline void put_str(uint8_t *p, size_t field, const char *s)
{
    memset(p, 0, field);
    if (s) {
        size_t n = strlen(s);
        memcpy(p, s, n < field ? n : field);
    }
}

#endif
