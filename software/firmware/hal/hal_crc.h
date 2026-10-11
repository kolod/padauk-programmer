/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CRC-32/ISO-HDLC acceleration. Platforms without a CRC unit forward to crc32_sw_update(). */
#ifndef PDK_HAL_CRC_H
#define PDK_HAL_CRC_H

#include <stddef.h>
#include <stdint.h>

/* Same contract as crc32_sw_update(): reflected running state, init 0xFFFFFFFF, no final xor. */
uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len);

#endif
