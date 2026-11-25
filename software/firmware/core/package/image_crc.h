/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chip content checksums (data-model ImageChecksum): image_crc_raw — CRC-32 of the user memory as is,
 * image_crc_norm — the same with the k of finished calibration footprints and the immediates of serial
 * number sites (exactly 8 `RET #imm` in a row; longer runs are tables and stay) set to 0. Streamed word
 * by word in address order, so a chip can be summed while it is read. */
#ifndef PDK_IMAGE_CRC_H
#define PDK_IMAGE_CRC_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"
#include "effective_image.h"

#define IMAGE_CRC_WINDOW 16u   /* lookahead: a BandGap footprint (14) / a serial run and the word after it (9) */

typedef struct {
    const chip_info_t *chip;
    uint16_t mask, ret_op;
    uint32_t raw, norm;
    uint16_t next_addr;            /* address of the next word fed */
    uint16_t base;                 /* address of win[0] */
    uint8_t n;
    bool prev_ret;                 /* the last word passed to the norm sum was RET #imm */
    uint16_t win[IMAGE_CRC_WINDOW];
} image_crc_t;

void image_crc_init(image_crc_t *c, const chip_info_t *chip);

/* Words in address order, starting at 0; every address up to code_words - 1 must be fed. */
void image_crc_feed(image_crc_t *c, uint16_t word);

void image_crc_finish(image_crc_t *c, uint32_t *raw, uint32_t *norm);

/* Sums of the effective image as it stands (calibration sites in their current form, serial number). */
void image_crc_of_eff(const eff_image_t *e, uint32_t *raw, uint32_t *norm);

#endif
