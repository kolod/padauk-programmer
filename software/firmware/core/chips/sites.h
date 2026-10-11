/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Word access shared by calibration/serial site search, validation and the effective image. */
#ifndef PDK_SITES_H
#define PDK_SITES_H

#include <stdbool.h>
#include <stdint.h>

/* Returns the word at `addr` and whether it is defined (undefined words read as 0xFFFF). */
typedef uint16_t (*word_get_fn)(void *ctx, uint16_t addr, bool *defined);

#endif
