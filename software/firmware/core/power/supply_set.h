/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Arbitrary VDD/VPP for checking the supply front end with a meter (contracts/protocol.md,
 * "Ручне керування живленням"): SUPPLY_SET applies the voltages with the FR-040 settling rule and
 * stored calibration (or, for diagnostics, without the tolerance check), SUPPLY_GET reports set points
 * and measurements with their spread. Nothing may be connected. */
#ifndef PDK_SUPPLY_SET_H
#define PDK_SUPPLY_SET_H

#include <stdbool.h>
#include <stdint.h>

void supply_set_init(void);

/* Session lost, idle too long or calibration started: supplies off. */
void supply_set_abort(void);
void supply_set_poll(void);
bool supply_set_active(void);

uint16_t supply_set_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t supply_set_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
