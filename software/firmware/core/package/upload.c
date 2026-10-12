/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "upload.h"

#include <string.h>

#include "header.h"
#include "pkg_cmds.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"
#include "storage/slots.h"
#include "validate.h"

typedef enum { UP_IDLE = 0, UP_RECEIVING, UP_COMPLETE } upload_state_t;

static struct {
    upload_state_t state;
    uint8_t slot;
    uint32_t expected;
    uint32_t received;
    uint8_t header_raw[PKG_HEADER_SIZE];
    pkg_header_t header;
    const chip_info_t *chip;
} up;

void upload_init(void) { memset(&up, 0, sizeof up); }
void upload_abort(void)
{
    up.state = UP_IDLE;
    slots_set_upload(SLOT_NONE);   /* pages written so far are erased in the background */
}
bool upload_active(void) { return up.state != UP_IDLE; }

uint16_t pkg_cmd_begin(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    upload_abort();
    if (len != PKG_HEADER_SIZE)
        return PDK_BAD_PAYLOAD;
    if (package_store_flags() & PDK_FLAG_MASTER_RESTORE_PENDING)
        return PDK_MASTER_RESTORE_PENDING;
    uint16_t st = pkg_header_parse(req, true, slots_size(), &up.header, &up.chip);
    if (st != PDK_OK)
        return st;
    memcpy(up.header_raw, req, PKG_HEADER_SIZE);
    up.slot = package_store_upload_slot();
    slots_set_upload(up.slot);
    up.expected = up.header.package_size;
    up.received = 0;
    up.state = UP_RECEIVING;
    put32(rsp, up.expected);
    *rsp_len = 4;
    return PDK_OK;
}

uint16_t pkg_cmd_data(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (up.state != UP_RECEIVING)
        return PDK_NOT_ALLOWED_NOW;
    if (len < 4) {
        upload_abort();
        return PDK_BAD_PAYLOAD;
    }
    uint32_t offset = le32(req);
    const uint8_t *data = req + 4;
    uint32_t n = len - 4u;
    if (offset != up.received) {
        put32(rsp, up.received);
        *rsp_len = 4;
        upload_abort();
        return PDK_PKG_OFFSET;
    }
    if ((n & 1u) || offset + n > up.expected) {
        upload_abort();
        return PDK_BAD_PAYLOAD;
    }
    /* The header inside the data must be the one announced in PKG_BEGIN. */
    if (offset < PKG_HEADER_SIZE) {
        uint32_t k = PKG_HEADER_SIZE - offset;
        if (k > n)
            k = n;
        if (memcmp(data, up.header_raw + offset, k) != 0) {
            upload_abort();
            return PDK_PKG_HEADER_INVALID;
        }
    }
    if (!slots_write(up.slot, offset, data, n)) {
        upload_abort();
        return PDK_STORE_WRITE_FAILED;
    }
    up.received += n;
    if (up.received == up.expected) {
        if (!slots_flush(up.slot)) {
            upload_abort();
            return PDK_STORE_WRITE_FAILED;
        }
        up.state = UP_COMPLETE;
    }
    put32(rsp, up.received);
    *rsp_len = 4;
    return PDK_OK;
}

uint16_t pkg_cmd_commit(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (up.state != UP_COMPLETE)
        return PDK_NOT_ALLOWED_NOW;
    upload_abort();   /* any outcome ends the upload; on success commit makes the slot active */

    /* CRC over what is really in flash. */
    uint32_t crc = slots_crc(up.slot, up.expected - 4u);
    uint8_t trailer[4];
    slots_read(up.slot, up.expected - 4u, trailer, 4);
    if (crc != le32(trailer)) {
        put16(rsp, 0xFFFF);
        *rsp_len = 2;
        return PDK_PKG_CRC_MISMATCH;
    }
    uint8_t hdr[PKG_HEADER_SIZE];
    slots_read(up.slot, 0, hdr, sizeof hdr);
    if (memcmp(hdr, up.header_raw, sizeof hdr) != 0) {
        put16(rsp, 0xFFFF);
        *rsp_len = 2;
        return PDK_PKG_HEADER_INVALID;
    }

    static pkg_slot_reader_t reader;
    pkg_slot_reader_init(&reader, up.slot, up.header.image_words);
    static pkg_plan_t plan;
    uint16_t bad = 0xFFFF;
    uint16_t st = pkg_validate(&up.header, up.chip, pkg_slot_word, &reader, &plan, &bad);
    if (st != PDK_OK) {
        put16(rsp, bad);
        *rsp_len = 2;
        return st;
    }
    if (!package_store_commit(up.slot, up.expected, crc))
        return PDK_STORE_WRITE_FAILED;

    put32(rsp, crc);
    rsp[4] = plan.calib_count;
    rsp[5] = plan.serial_count;
    put16(rsp + 6, plan.effective_words);
    *rsp_len = 8;
    pkg_send_state_event();
    return PDK_OK;
}

uint16_t pkg_cmd_abort(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    upload_abort();
    return PDK_OK;
}
