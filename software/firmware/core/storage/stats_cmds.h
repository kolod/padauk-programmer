/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PDK_STATS_CMDS_H
#define PDK_STATS_CMDS_H

#include <stdint.h>

uint16_t stats_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t stats_cmd_reset(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
