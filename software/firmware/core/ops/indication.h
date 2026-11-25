/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Indication states (contracts/chip-engine.md §4, FR-054) derived from the operations: READY, BUSY with
 * stage and percent, SUCCESS, FAIL with the class of the cause and the status, BLOCKED (a local start
 * would run SELF_TEST instead). Passed to the platform through hal_ui_state(). */
#ifndef PDK_INDICATION_H
#define PDK_INDICATION_H

#include <stdint.h>

#include "hal_ui.h"

void indication_busy(uint8_t stage, uint8_t percent);
void indication_done(uint8_t op_type, uint16_t status);
void indication_fail(uint16_t status);   /* a local request refused without an operation */

/* The class of a failure status (STORAGE, NO_PACKAGE, CHIP, CALIB, POWER_SELFTEST, MASTER_PENDING). */
pdk_ui_fail_class_t indication_class(uint16_t status);

#endif
