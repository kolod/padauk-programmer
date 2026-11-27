/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Diagnostic trace (protocol 1.1, TRACE_SET / EVT_TRACE): while the PC asks for it, the core and the chip engine report
 * what they do with the supplies and the IC — every voltage applied with its measured value, entering and leaving
 * programming mode, the raw ACK / ID answers of the IC, the probe attempts. Off by default and on every session reset;
 * costs nothing when off. */
#ifndef PDK_TRACE_H
#define PDK_TRACE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    TRACE_POWER_OK = 1,     /* b8 rail, w1 set mV, w2 measured mV, d elapsed µs */
    TRACE_POWER_FAULT = 2,  /* b8 rail, w1 set mV, w2 last measured mV, d elapsed µs */
    TRACE_ENTER = 3,        /* b8 family, w1 VPP mV, w2 VDD mV, d mode */
    TRACE_LEAVE = 4,        /* b8 mode, w1 the pause before the next enter, ms */
    TRACE_ACK = 5,          /* b8 family, w1 expected ID, w2 bits, d raw answer */
    TRACE_GETID = 6,        /* b8 family, w1 expected ID, w2 bits, d raw answer (last 32 bits) */
    TRACE_PROBE = 7,        /* b8 family, w2 bits, d raw answer */
    TRACE_CALIB = 8,        /* b8 trim value, w1 counted edges, w2 the independent count (0 none), d SYSCLK Hz */
    TRACE_CALIB_CHECK = 9,  /* b8 attempt, w1 trim value, w2 deviation from the search, 0.01 %, d SYSCLK Hz */
};

#define TRACE_RECORD_SIZE 14u   /* u32 t_us, u8 kind, u8 b8, u16 w1, u16 w2, u32 d */

void trace_set(bool on);
bool trace_on(void);
void trace_emit(uint8_t kind, uint8_t b8, uint16_t w1, uint16_t w2, uint32_t d);

/* TRACE_SET handler: u8 on (0/1). */
uint16_t trace_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
