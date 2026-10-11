/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Decoded frame: type, flags, seq, code, status, len, payload[len], crc32 (contracts/protocol.md §1). */
#ifndef PDK_FRAME_H
#define PDK_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t code;
    uint16_t status;
    uint16_t len;
    const uint8_t *payload;
} pdk_frame_t;

/* Writes the decoded frame (header + payload + CRC) into `out`. Returns its size or 0 if `cap` is too small. */
size_t frame_encode(const pdk_frame_t *f, uint8_t *out, size_t cap);

/* Parses a decoded frame. Returns PDK_OK or PDK_FRAME_ERROR (length, CRC, payload limit). */
uint16_t frame_parse(const uint8_t *buf, size_t len, size_t max_payload, pdk_frame_t *f);

#endif
