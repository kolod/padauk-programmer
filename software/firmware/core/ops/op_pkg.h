/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The active package as the operations use it: header, PKG_COMMIT plan, slot reader, effective image. */
#ifndef PDK_OP_PKG_H
#define PDK_OP_PKG_H

#include <stdbool.h>
#include <stdint.h>

#include "package/effective_image.h"
#include "package/header.h"
#include "package/validate.h"

typedef struct {
    const chip_info_t *chip;
    pkg_header_t hdr;
    pkg_plan_t plan;
    pkg_slot_reader_t reader;
    eff_image_t eff;
    uint16_t mask;
} op_pkg_t;

/* One instance, shared by the operations (one runs at a time). */
extern op_pkg_t op_pkg;

/* PKG_CHECK: STORE_EMPTY / STORE_CORRUPTED, or PDK_OK with op_pkg filled. */
uint16_t op_pkg_load(void);

/* Words that blank checks, verification and checksums look at: not reserved, not the excluded first word. */
bool op_pkg_counted(const chip_info_t *chip, uint16_t addr);

#endif
