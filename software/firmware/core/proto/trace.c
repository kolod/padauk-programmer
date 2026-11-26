/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "trace.h"

#include "hal_time.h"
#include "le.h"
#include "proto_defs.h"
#include "session.h"

static bool on;

void trace_set(bool enable) { on = enable; }
bool trace_on(void) { return on; }

void trace_emit(uint8_t kind, uint8_t b8, uint16_t w1, uint16_t w2, uint32_t d)
{
    if (!on)
        return;
    uint8_t r[TRACE_RECORD_SIZE];
    put32(r, hal_time_us());
    r[4] = kind;
    r[5] = b8;
    put16(r + 6, w1);
    put16(r + 8, w2);
    put32(r + 10, d);
    session_send_event(PDK_EVT_TRACE, r, sizeof r);   /* dropped without a session */
}

uint16_t trace_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    (void)rsp;
    (void)rsp_len;
    if (len != 1 || req[0] > 1)
        return PDK_BAD_PAYLOAD;
    on = req[0] != 0;
    return PDK_OK;
}
