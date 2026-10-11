/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_proto.h"

#include <string.h>

#include "fake_hal.h"
#include "pdk_config.h"
#include "pdk_core.h"
#include "proto/cobs.h"
#include "proto/proto_defs.h"

void tp_send_raw(const void *bytes, size_t len) { fake_rx_push(bytes, len); }

void tp_send(uint16_t seq, uint16_t code, const void *payload, uint16_t len)
{
    static uint8_t frame[1200], wire[1300];
    pdk_frame_t f = {.type = PDK_FT_REQ, .seq = seq, .code = code, .len = len, .payload = payload};
    size_t n = frame_encode(&f, frame, sizeof frame);
    size_t w = cobs_encode(frame, n, wire, sizeof wire);
    fake_rx_push(wire, w);
}

int tp_collect(tp_frame_t *out, int max)
{
    for (int i = 0; i < 64; i++)
        pdk_core_poll();
    static uint8_t tx[70000];
    size_t n = fake_tx_take(tx, sizeof tx);
    int count = 0;
    size_t start = 0;
    static uint8_t dec[1200];
    for (size_t i = 0; i < n && count < max; i++) {
        if (tx[i] != 0)
            continue;
        int d = cobs_decode(tx + start, i - start, dec, sizeof dec);
        start = i + 1;
        pdk_frame_t f;
        if (d < 0 || frame_parse(dec, (size_t)d, 1024, &f) != PDK_OK)
            continue;
        out[count].type = f.type;
        out[count].seq = f.seq;
        out[count].code = f.code;
        out[count].status = f.status;
        out[count].len = f.len;
        memcpy(out[count].payload, f.payload, f.len);
        count++;
    }
    return count;
}

int tp_call(uint16_t seq, uint16_t code, const void *payload, uint16_t len, tp_frame_t *rsp)
{
    static tp_frame_t frames[32];
    tp_send(seq, code, payload, len);
    int n = tp_collect(frames, 32);
    for (int i = 0; i < n; i++) {
        if (frames[i].type == PDK_FT_RSP || frames[i].type == PDK_FT_NAK) {
            *rsp = frames[i];
            return 0;
        }
    }
    return -1;
}

int tp_upload(const uint8_t *pkg, size_t len, uint16_t *seq, uint16_t chunk, tp_frame_t *rsp)
{
    if (tp_call((*seq)++, PDK_CMD_PKG_BEGIN, pkg, 128, rsp) || rsp->status != PDK_OK)
        return TP_STAGE_BEGIN;
    static uint8_t buf[4 + PDK_MAX_PAYLOAD];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < chunk ? len - off : chunk;
        buf[0] = (uint8_t)off;
        buf[1] = (uint8_t)(off >> 8);
        buf[2] = (uint8_t)(off >> 16);
        buf[3] = (uint8_t)(off >> 24);
        memcpy(buf + 4, pkg + off, n);
        if (tp_call((*seq)++, PDK_CMD_PKG_DATA, buf, (uint16_t)(4 + n), rsp) || rsp->status != PDK_OK)
            return TP_STAGE_DATA;
        off += n;
    }
    if (tp_call((*seq)++, PDK_CMD_PKG_COMMIT, NULL, 0, rsp) || rsp->status != PDK_OK)
        return TP_STAGE_COMMIT;
    return TP_STAGE_OK;
}

void tp_hello(uint16_t seq)
{
    static tp_frame_t r;
    uint8_t p[] = {1, 0, 't'};
    tp_call(seq, PDK_CMD_HELLO, p, sizeof p, &r);
}
