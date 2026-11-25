/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CONFIG_GET / CONFIG_SET: calibration modes of a local COPY (contracts/protocol.md §4), in the journal. */
#ifndef PDK_CONFIG_CMDS_H
#define PDK_CONFIG_CMDS_H

#include <stdint.h>

uint16_t config_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t config_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
