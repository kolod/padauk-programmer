/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "frame.h"

#include <string.h>

#include "crc32.h"
#include "le.h"
#include "pdk_config.h"
#include "proto_defs.h"

size_t frame_encode(const pdk_frame_t *f, uint8_t *out, size_t cap)
{
    size_t total = PDK_FRAME_OVERHEAD + f->len;
    if (cap < total)
        return 0;
    out[0] = f->type;
    out[1] = f->flags;
    put16(out + 2, f->seq);
    put16(out + 4, f->code);
    put16(out + 6, f->status);
    put16(out + 8, f->len);
    if (f->len)
        memmove(out + 10, f->payload, f->len);
    put32(out + 10 + f->len, crc32_compute(out, 10u + f->len));
    return total;
}

uint16_t frame_parse(const uint8_t *buf, size_t len, size_t max_payload, pdk_frame_t *f)
{
    if (len < PDK_FRAME_OVERHEAD)
        return PDK_FRAME_ERROR;
    uint16_t plen = le16(buf + 8);
    if (plen > max_payload || len != PDK_FRAME_OVERHEAD + plen)
        return PDK_FRAME_ERROR;
    if (crc32_compute(buf, 10u + plen) != le32(buf + 10 + plen))
        return PDK_FRAME_ERROR;
    f->type = buf[0];
    f->flags = buf[1];
    f->seq = le16(buf + 2);
    f->code = le16(buf + 4);
    f->status = le16(buf + 6);
    f->len = plen;
    f->payload = buf + 10;
    return PDK_OK;
}
