/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CRC-32/ISO-HDLC (zlib.crc32): poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF. */
#ifndef PDK_CRC32_H
#define PDK_CRC32_H

#include <stddef.h>
#include <stdint.h>

#define CRC32_INIT 0xFFFFFFFFu

/* Software implementation (nibble table). `state` starts at CRC32_INIT. */
uint32_t crc32_sw_update(uint32_t state, const void *data, size_t len);

/* Incremental API; uses the hardware unit through hal_crc32_update() when available. */
static inline uint32_t crc32_init(void) { return CRC32_INIT; }
uint32_t crc32_update(uint32_t state, const void *data, size_t len);
static inline uint32_t crc32_final(uint32_t state) { return state ^ 0xFFFFFFFFu; }

/* One-shot convenience. */
uint32_t crc32_compute(const void *data, size_t len);

#endif
