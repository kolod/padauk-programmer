/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a free-pdk calibration placeholder becomes in the chip (research R8, R9): the measurement loop
 * written in its place, the finished form after calibration (`MOV A,#k` + trim register write, NOPs
 * elsewhere) and back — recognising finished footprints and loops in a chip dump and restoring the
 * placeholder from them. */
#ifndef PDK_CALIB_FOOTPRINT_H
#define PDK_CALIB_FOOTPRINT_H

#include <stdbool.h>
#include <stdint.h>

#include "calib_sites.h"
#include "chip_table.h"

enum { CALIB_FORM_FOOTPRINT = 1, CALIB_FORM_LOOP = 2 };

#define CALIB_LOOP_IMM 0xFFu   /* initial result register of a fresh loop: the first ADD A,1 gives 0 */

typedef struct {
    uint8_t form;   /* CALIB_FORM_* */
    uint8_t kind;   /* CALIB_KIND_* */
    uint8_t k;      /* MOV A,#k of the footprint; for a loop, the immediate of its MOV A,#… */
    uint8_t len;    /* CALIB_RC_LEN or CALIB_BG_LEN */
} calib_match_t;

/* Measurement loop for `site` at site->addr; `imm` is the immediate of its MOV A,#… (CALIB_LOOP_IMM,
 * or k − 1 to start counting from k). Returns the length, 0 if the core width has no loop. */
uint8_t calib_loop_words(uint8_t code_bits, const calib_site_t *site, uint8_t imm, uint16_t *out);

/* Finished form with the calibration result `k`; every word only clears bits of the loop. */
uint8_t calib_final_words(uint8_t code_bits, const calib_site_t *site, uint8_t k, uint16_t *out);

/* The placeholder (AND A,# …) describing `site`, as the free-pdk macros emit it. */
uint8_t calib_placeholder_words(uint8_t code_bits, const calib_site_t *site, uint16_t *out);

/* True if words w[0 .. avail) at address `addr` start a finished footprint or a measurement loop of
 * `chip`: register addresses must be the chip's (they also give IHRC vs ILRC), a loop's GOTO must
 * point into itself. */
bool calib_match(const chip_info_t *chip, const uint16_t *w, unsigned avail, uint16_t addr, calib_match_t *m);

/* Site for a recognised footprint/loop at `addr`, registers from the chip, with the given target. */
void calib_site_from_match(const chip_info_t *chip, uint16_t addr, const calib_match_t *m, uint32_t target_hz,
                           uint16_t mV, calib_site_t *site);

/* All footprints and loops in words [0, count) (non-overlapping, in address order). Returns the number
 * found, at most `max`. */
uint8_t calib_scan(const chip_info_t *chip, word_get_fn get, void *ctx, uint16_t count, uint16_t *addrs,
                   calib_match_t *matches, uint8_t max);

#endif
