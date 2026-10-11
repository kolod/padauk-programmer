/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Package upload state machine IDLE → RECEIVING → COMPLETE → (commit) IDLE (data-model Upload). */
#ifndef PDK_UPLOAD_H
#define PDK_UPLOAD_H

#include <stdbool.h>
#include <stdint.h>

void upload_init(void);
void upload_abort(void);
bool upload_active(void);

uint16_t pkg_cmd_begin(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_data(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_commit(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
uint16_t pkg_cmd_abort(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
