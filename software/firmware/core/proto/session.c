/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"

#include <string.h>

#include "cobs.h"
#include "dispatch.h"
#include "frame.h"
#include "hal_transport.h"
#include "pdk_config.h"
#include "pdk_core.h"
#include "proto_defs.h"

static struct {
    bool open;
    bool established;
    bool have_cache;
    uint16_t last_seq;
    uint16_t evt_seq;
    cobs_rx_t rx;
    uint8_t rx_raw[PDK_MAX_ENCODED];
    uint8_t rx_frame[PDK_MAX_FRAME];
    uint8_t rsp_payload[PDK_MAX_PAYLOAD];
    uint8_t cache[PDK_MAX_FRAME];       /* last decoded RSP frame */
    size_t cache_len;
    uint8_t tx_frame[PDK_MAX_FRAME];
    uint8_t tx_wire[PDK_MAX_ENCODED];
} s;

static void send_decoded(const uint8_t *frame, size_t len)
{
    size_t n = cobs_encode(frame, len, s.tx_wire, sizeof s.tx_wire);
    if (n)
        hal_transport_write(s.tx_wire, n);
}

static void send_frame(const pdk_frame_t *f, bool cache)
{
    size_t n = frame_encode(f, s.tx_frame, sizeof s.tx_frame);
    if (!n)
        return;
    if (cache) {
        memcpy(s.cache, s.tx_frame, n);
        s.cache_len = n;
        s.have_cache = true;
    }
    send_decoded(s.tx_frame, n);
}

static void send_nak(void)
{
    pdk_frame_t f = {.type = PDK_FT_NAK, .seq = 0xFFFF, .code = 0, .status = PDK_FRAME_ERROR, .len = 0};
    send_frame(&f, false);
}

static void reset_session(void)
{
    s.established = false;
    s.have_cache = false;
    s.cache_len = 0;
    s.rx.len = 0;
    s.rx.overflow = false;
    pdk_core_on_session_reset();
}

void session_init(void)
{
    memset(&s, 0, sizeof s);
    cobs_rx_init(&s.rx, s.rx_raw, sizeof s.rx_raw);
    s.open = hal_transport_is_open();
}

bool session_is_open(void) { return s.open; }
bool session_is_established(void) { return s.established; }
void session_set_established(bool established) { s.established = established; }

static void handle_frame(const uint8_t *buf, size_t len)
{
    pdk_frame_t req;
    if (frame_parse(buf, len, PDK_MAX_PAYLOAD, &req) != PDK_OK || req.type != PDK_FT_REQ) {
        send_nak();
        return;
    }
    if (s.have_cache && req.seq == s.last_seq) {
        send_decoded(s.cache, s.cache_len);   /* duplicate: same answer, no re-execution */
        return;
    }
    uint16_t rsp_len = 0;
    uint16_t status = dispatch_request(req.code, req.payload, req.len, s.rsp_payload, &rsp_len);
    pdk_frame_t rsp = {.type = PDK_FT_RSP, .seq = req.seq, .code = req.code, .status = status,
                       .len = rsp_len, .payload = s.rsp_payload};
    s.last_seq = req.seq;
    send_frame(&rsp, true);
}

void session_rx_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t flen = 0;
        cobs_rx_result_t r = cobs_rx_byte(&s.rx, data[i], s.rx_frame, sizeof s.rx_frame, &flen);
        if (r == COBS_RX_FRAME)
            handle_frame(s.rx_frame, flen);
        else if (r == COBS_RX_ERROR)
            send_nak();
    }
}

void session_poll(void)
{
    bool open = hal_transport_is_open();
    if (open != s.open) {
        s.open = open;
        reset_session();
    }
    uint8_t buf[64];
    size_t n;
    while ((n = hal_transport_read(buf, sizeof buf)) > 0)
        session_rx_bytes(buf, n);
}

void session_send_event(uint16_t code, const uint8_t *payload, uint16_t len)
{
    if (!s.open || len > PDK_MAX_PAYLOAD)
        return;
    pdk_frame_t f = {.type = PDK_FT_EVT, .seq = s.evt_seq++, .code = code, .status = 0, .len = len,
                     .payload = payload};
    send_frame(&f, false);
}
