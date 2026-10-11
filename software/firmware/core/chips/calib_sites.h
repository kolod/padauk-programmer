/* SPDX-License-Identifier: GPL-3.0-or-later */
/* free-pdk calibration placeholders (research R8). */
#ifndef PDK_CALIB_SITES_H
#define PDK_CALIB_SITES_H

#include <stdint.h>

#include "sites.h"

#define CALIB_MAX_SITES 16u
#define CALIB_RC_LEN 10u
#define CALIB_BG_LEN 14u
#define CALIB_UNKNOWN_RUN 6u

enum { CALIB_KIND_IHRC = 1, CALIB_KIND_ILRC = 2, CALIB_KIND_BG = 3 };

typedef struct {
    uint16_t addr;
    uint8_t kind;
    uint8_t len;
    uint32_t target_hz;      /* IHRC/ILRC */
    uint16_t mV;
    uint8_t reg0;            /* IHRCR/ILRCR (RC) or BGTR (BG) */
    uint8_t reg1, reg2;      /* GPCC, GPCS (BG) */
} calib_site_t;

/* `AND A,#imm` opcode base for the core width (0 if unsupported). */
uint16_t calib_and_opcode(uint8_t code_bits);

/* Finds all placeholders in words [0, count). Returns PDK_OK, PDK_PKG_CALIB_UNKNOWN (with *bad_addr)
 * or PDK_BAD_PAYLOAD if more than CALIB_MAX_SITES are present. */
uint16_t calib_find_sites(uint8_t code_bits, word_get_fn get, void *ctx, uint16_t count,
                          calib_site_t *sites, uint8_t *n_sites, uint16_t *bad_addr);

#endif
