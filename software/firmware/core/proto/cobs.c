/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cobs.h"

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t need = len + len / 254u + 2u;
    if (cap < need)
        return 0;

    size_t code_pos = 0;
    size_t o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            code++;
            if (code == 0xFF) {
                out[code_pos] = code;
                code_pos = o++;
                code = 1;
            }
        }
    }
    out[code_pos] = code;
    out[o++] = 0x00;
    return o;
}

int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0)
            return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len || in[i] == 0 || o >= cap)
                return -1;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < len) {
            if (o >= cap)
                return -1;
            out[o++] = 0;
        }
    }
    return (int)o;
}

void cobs_rx_init(cobs_rx_t *rx, uint8_t *buf, size_t cap)
{
    rx->buf = buf;
    rx->cap = cap;
    rx->len = 0;
    rx->overflow = false;
}

cobs_rx_result_t cobs_rx_byte(cobs_rx_t *rx, uint8_t byte, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (byte != 0) {
        if (rx->len < rx->cap)
            rx->buf[rx->len++] = byte;
        else
            rx->overflow = true;
        return COBS_RX_NONE;
    }

    /* delimiter */
    size_t n = rx->len;
    bool overflow = rx->overflow;
    rx->len = 0;
    rx->overflow = false;
    if (overflow)
        return COBS_RX_ERROR;
    if (n == 0)
        return COBS_RX_NONE;

    int d = cobs_decode(rx->buf, n, out, out_cap);
    if (d < 0)
        return COBS_RX_ERROR;
    *out_len = (size_t)d;
    return COBS_RX_FRAME;
}
