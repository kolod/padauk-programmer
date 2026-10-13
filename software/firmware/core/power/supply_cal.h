/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supply calibration with an external meter (contracts/protocol.md, "Калібрування живлення"):
 * CAL_START raises VDD/VPP to the calibration points, CAL_SET takes the voltages the user measured,
 * derives the measurement and output scale of each rail and applies exact 6 V / 12 V for checking,
 * CAL_CONFIRM stores or discards the result. */
#ifndef PDK_SUPPLY_CAL_H
#define PDK_SUPPLY_CAL_H

#include <stdbool.h>
#include <stdint.h>

/* Loads the stored calibration (journal) into the power layer. */
void supply_cal_init(void);

/* Session lost or idle too long: supplies off, the previous calibration stays in effect. */
void supply_cal_abort(void);
void supply_cal_poll(void);
bool supply_cal_active(void);

uint16_t supply_cal_cmd_start(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t supply_cal_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t supply_cal_cmd_confirm(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t supply_cal_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t supply_cal_cmd_clear(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
