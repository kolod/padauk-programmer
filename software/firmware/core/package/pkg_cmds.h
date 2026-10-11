/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PDK_PKG_CMDS_H
#define PDK_PKG_CMDS_H

#include <stdint.h>

#include "upload.h"

uint16_t pkg_cmd_info(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_read(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_delete(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_check(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

/* EVT_PKG_STATE: u8 pkg_state, u32 package_crc32. */
void pkg_send_state_event(void);

#endif
