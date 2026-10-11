/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PDK_DISPATCH_H
#define PDK_DISPATCH_H

#include <stdint.h>

/* Command handler: fills `rsp` (capacity PDK_MAX_PAYLOAD) and `*rsp_len`, returns a status code. */
typedef uint16_t (*pdk_handler_fn)(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

enum {
    PDK_CMDF_NO_SESSION = 1u << 0,   /* allowed before HELLO */
    PDK_CMDF_DURING_OP = 1u << 1,    /* allowed while an operation runs */
};

typedef struct {
    uint16_t code;
    uint8_t flags;
    pdk_handler_fn fn;
} pdk_command_t;

/* Routes a request; applies session, BUSY and unknown-command rules. */
uint16_t dispatch_request(uint16_t code, const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

/* Builds the INFO structure (contracts/protocol.md §4); returns its length. */
uint16_t dispatch_build_info(uint8_t *out);

#endif
