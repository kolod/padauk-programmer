/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Two package slots in internal flash (research R5). Pages are erased lazily while writing. */
#ifndef PDK_SLOTS_H
#define PDK_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t slots_size(void);
void slots_read(uint8_t slot, uint32_t offset, uint8_t *buf, size_t len);

/* Writes `len` bytes at `offset`; erases each page the first time it is touched (tracked in
 * *erased, one bit per page). Offset and length must be even. */
bool slots_write(uint8_t slot, uint32_t offset, const uint8_t *data, size_t len, uint32_t *erased);

/* CRC-32 of the first `len` bytes of the slot. */
uint32_t slots_crc(uint8_t slot, uint32_t len);

#endif
