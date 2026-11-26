/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RUN with the UART bridge: the session hook and the UART_TX command (op_run_run is in op_manager.h). */
#ifndef PDK_OP_RUN_H
#define PDK_OP_RUN_H

#include <stdbool.h>
#include <stdint.h>

/* Called on every session reset: a running RUN stops with HOST_DISCONNECTED. */
void op_run_session_reset(void);

/* True while RUN has its bridge up (STATUS.state = running). */
bool op_run_active(void);

/* UART_TX: bytes to the chip; NOT_ALLOWED_NOW outside RUN, BUSY when they cannot be queued now. */
uint16_t op_run_cmd_uart_tx(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
