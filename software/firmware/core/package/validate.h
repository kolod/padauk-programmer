/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Full package check at PKG_COMMIT (contracts/package-format.md, "У PKG_COMMIT"). */
#ifndef PDK_VALIDATE_H
#define PDK_VALIDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/calib_sites.h"
#include "chips/serial_sites.h"
#include "header.h"

typedef struct {
    uint8_t calib_count;
    calib_site_t calib[CALIB_MAX_SITES];
    uint8_t serial_count;
    uint16_t serial_addr[SERIAL_MAX_SITES];
    bool fuse_defined;
    uint16_t fuse;              /* effective fuse (header overrides image) */
    uint16_t effective_words;   /* words that will be written, without the fuse */
} pkg_plan_t;

/* Reads package words from a slot (words at offset 128, mask after them). The whole mask and a
 * window of words are cached: the checks visit every word several times and flash may be slow
 * (SPI NOR). */
#define PKG_READER_MASK_MAX 512u    /* 2 · ceil(4096 / 16) bytes */
#define PKG_READER_WINDOW 128u      /* words */

typedef struct {
    uint8_t slot;
    uint16_t image_words;
    uint32_t mask_offset;
    uint8_t mask[PKG_READER_MASK_MAX];
    uint16_t win_first;             /* first cached word, 0xFFFF = none */
    uint16_t win[PKG_READER_WINDOW];
} pkg_slot_reader_t;

void pkg_slot_reader_init(pkg_slot_reader_t *r, uint8_t slot, uint16_t image_words);
uint16_t pkg_slot_word(void *reader, uint16_t addr, bool *defined);

/* Checks reserved areas, fuse conflict/security and placeholders. On error *bad_addr is the word
 * address (0xFFFF if not applicable). */
uint16_t pkg_validate(const pkg_header_t *h, const chip_info_t *c, word_get_fn get, void *ctx,
                      pkg_plan_t *plan, uint16_t *bad_addr);

#endif
