/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Random 64-bit serial numbers (research R18): raw noise from hal_entropy, health-checked, mixed with
 * the MCU UID, the journal record number and a RAM counter through BLAKE2s. */
#ifndef PDK_SERIAL_RNG_H
#define PDK_SERIAL_RNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERIAL_RAW_BYTES 64u   /* 512 raw bits: >= 256 bits of entropy even at half a bit per raw bit */

/* Simplified NIST SP 800-90B start-up checks on raw samples: repetition count, adaptive proportion and
 * a short period. False = the source is broken and must not be used. */
bool serial_rng_health(const uint8_t *raw, size_t len);

/* BLAKE2s-256 (RFC 7693), unkeyed. */
void blake2s_256(const void *in, size_t len, uint8_t out[32]);

/* New serial number, little-endian. PDK_OK, or PDK_INTERNAL if the entropy source failed (no number:
 * the operation must not start rather than write a predictable one). */
uint16_t serial_rng_next(uint32_t rec_seq, uint8_t out[8]);

#endif
