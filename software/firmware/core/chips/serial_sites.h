/* SPDX-License-Identifier: GPL-3.0-or-later */
/* free-pdk serial number placeholder: RET 'F','P','S','E','R','I','A','L' (EASY_PDK_SERIAL). */
#ifndef PDK_SERIAL_SITES_H
#define PDK_SERIAL_SITES_H

#include <stdint.h>

#include "sites.h"

#define SERIAL_SITE_LEN 8u
#define SERIAL_MAX_SITES 4u

/* `RET #imm` opcode base for the core width. */
uint16_t serial_ret_opcode(uint8_t code_bits);

/* Finds placeholder positions in [0, count); returns the number found (at most `max`). */
uint8_t serial_find_sites(uint8_t code_bits, word_get_fn get, void *ctx, uint16_t count,
                          uint16_t *addrs, uint8_t max);

#endif
