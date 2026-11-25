/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test helper: builds a firmware package (contracts/package-format.md) from words and a defined mask. */
#ifndef PDK_TEST_PKG_H
#define PDK_TEST_PKG_H

#include <stddef.h>
#include <stdint.h>

#include "chips/chip_table.h"

#define TPK_MAX_SIZE (128u + 2u * 0x1000u + 2u * 0x100u + 4u)

/* def[i] != 0: word i is set. Returns the package size. */
size_t tpk_build(uint8_t *out, const chip_info_t *c, const uint16_t *w, const uint8_t *def, uint16_t n,
                 uint32_t flags, uint16_t fuse);

#endif
