/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Rewriting a FLASH master from the active package (its dump): MEASURE_MASTER and the restore. The caller
 * has loaded op_pkg (op_pkg_load) and checked the chip in the socket. */
#ifndef PDK_OP_MASTER_H
#define PDK_OP_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "chip_engine.h"
#include "chips/calib_sites.h"

/* Erase, then the dump with `site` replaced by a measurement loop that starts counting at `start_value`
 * (MOV A,#(start_value − 1)); no fuse. */
uint16_t master_write_loop(const calib_site_t *site, uint8_t start_value, chip_poll_fn poll);

/* Erase, the dump, verify, the fuse; then MASTER_RESTORE_PENDING is cleared. `stages`: report ERASE /
 * WRITE / VERIFY / FUSE (the RESTORE_MASTER operation) instead of staying in the caller's stage. */
uint16_t master_restore(bool stages, chip_poll_fn poll);

#endif
