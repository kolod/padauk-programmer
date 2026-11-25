/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OP_START, OP_CANCEL, OP_RESULT and GET_STATUS (contracts/protocol.md §4). */
#ifndef PDK_OP_CMDS_H
#define PDK_OP_CMDS_H

#include <stdint.h>

uint16_t op_cmd_start(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t op_cmd_cancel(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t op_cmd_result(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t op_cmd_status(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
