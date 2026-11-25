/* SPDX-License-Identifier: GPL-3.0-or-later */
/* READ (contracts/protocol.md §4, data-model ReadResult): the chip's raw words go into the inactive slot,
 * RESULT_READ hands them out until the next PKG_BEGIN, READ or COPY. */
#ifndef PDK_OP_READ_H
#define PDK_OP_READ_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"

/* The read result stops being valid (its slot is about to be reused). */
void op_read_invalidate(void);

/* Reads the whole user memory of `chip` into the inactive slot (raw u16 LE words), with the checksums
 * and data_crc32 in RESULT; CHIP_PROTECTED (R10) leaves no result. Used by READ and COPY. */
uint16_t op_read_chip(const chip_info_t *chip, uint8_t *slot);

/* The chip type of READ / COPY: OP_START chip_id12, or 0 = what PROBE finds. Probes the chip
 * (stage ID) either way. */
uint16_t op_read_identify(const chip_info_t **chip);

uint16_t op_cmd_result_read(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
