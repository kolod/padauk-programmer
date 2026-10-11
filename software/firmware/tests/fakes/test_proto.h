/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test helpers: send requests through the fake transport and collect decoded frames. */
#ifndef PDK_TEST_PROTO_H
#define PDK_TEST_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "proto/frame.h"

typedef struct {
    uint8_t type;
    uint16_t seq, code, status, len;
    uint8_t payload[1100];
} tp_frame_t;

/* Encodes a REQ frame (COBS + delimiter) into the fake RX queue. */
void tp_send(uint16_t seq, uint16_t code, const void *payload, uint16_t len);

/* Raw bytes into the RX queue (for corruption tests). */
void tp_send_raw(const void *bytes, size_t len);

/* Runs pdk_core_poll() until the RX queue is drained, then decodes every captured frame.
 * Returns the number of frames written to `out`. */
int tp_collect(tp_frame_t *out, int max);

/* Sends one request and returns its response (the first RSP/NAK frame); events are skipped.
 * Returns 0 on success, -1 if no response was produced. */
int tp_call(uint16_t seq, uint16_t code, const void *payload, uint16_t len, tp_frame_t *rsp);

/* Full upload: PKG_BEGIN, PKG_DATA in `chunk`-byte pieces (even), PKG_COMMIT. *seq advances.
 * Returns the stage that failed (rsp holds its response) or TP_STAGE_OK (rsp = COMMIT result). */
enum { TP_STAGE_OK = 0, TP_STAGE_BEGIN, TP_STAGE_DATA, TP_STAGE_COMMIT };
int tp_upload(const uint8_t *pkg, size_t len, uint16_t *seq, uint16_t chunk, tp_frame_t *rsp);

/* HELLO with protocol 1.0 (establishes the session). */
void tp_hello(uint16_t seq);

#endif
