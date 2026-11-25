/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "op_cmds.h"

#include <string.h>

#include "op_manager.h"
#include "package/upload.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"

uint16_t op_cmd_start(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len < 6)
        return PDK_BAD_PAYLOAD;
    if (len - 6u > OP_EXT_MAX)
        return PDK_BAD_PAYLOAD;
    static op_request_t r;   /* ext makes it large */
    r = (op_request_t){.op_type = req[0], .op_flags = req[1], .source = PDK_SOURCE_HOST,
                       .chip_id12 = le16(req + 2), .param = le16(req + 4), .ext_len = (uint16_t)(len - 6u)};
    memcpy(r.ext, req + 6, r.ext_len);
    uint16_t id = 0;
    uint16_t st = op_manager_start(&r, &id);
    if (st == PDK_OK) {
        put16(rsp, id);
        *rsp_len = 2;
    }
    return st;
}

uint16_t op_cmd_cancel(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 2)
        return PDK_BAD_PAYLOAD;
    return op_manager_cancel(le16(req)) ? PDK_OK : PDK_NOT_ALLOWED_NOW;
}

uint16_t op_cmd_result(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 2)
        return PDK_BAD_PAYLOAD;
    const op_result_t *r = op_manager_result(le16(req));
    if (!r)
        return PDK_NOT_ALLOWED_NOW;   /* still running, or no such operation */
    *rsp_len = op_result_encode(r, rsp);
    return PDK_OK;
}

uint16_t op_cmd_status(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    op_status_t s;
    op_manager_status(&s);
    const journal_state_t *j = journal_get();
    rsp[0] = s.running ? PDK_STATE_OPERATION : upload_active() ? PDK_STATE_UPLOAD : PDK_STATE_IDLE;
    put16(rsp + 1, s.op_id);
    rsp[3] = s.op_type;
    rsp[4] = s.stage;
    rsp[5] = s.percent_total;
    rsp[6] = package_store_state();
    put32(rsp + 7, package_store_crc());
    put32(rsp + 11, j->ok_count);
    put32(rsp + 15, j->fail_count);
    rsp[19] = (uint8_t)(j->flags & PDK_FLAG_MASTER_RESTORE_PENDING);
    if (op_selftest_state() != PDK_SELFTEST_PASSED)   /* a local start would run SELF_TEST instead (R16) */
        rsp[19] |= PDK_FLAG_LOCAL_START_BLOCKED;
    rsp[20] = j->cfg_copy_ihrc;
    rsp[21] = j->cfg_copy_ilrc;
    rsp[22] = j->cfg_copy_bg;
    rsp[23] = op_selftest_state();
    *rsp_len = 24;
    return PDK_OK;
}
