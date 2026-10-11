/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pkg_cmds.h"

#include <string.h>

#include "header.h"
#include "pdk_config.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "proto/session.h"
#include "storage/package_store.h"

void pkg_send_state_event(void)
{
    uint8_t ev[5];
    ev[0] = package_store_state();
    put32(ev + 1, package_store_crc());
    session_send_event(PDK_EVT_PKG_STATE, ev, sizeof ev);
}

uint16_t pkg_cmd_info(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    rsp[0] = package_store_state();
    put32(rsp + 1, package_store_crc());
    if (rsp[0] == PDK_PKG_EMPTY)
        memset(rsp + 5, 0, PKG_HEADER_SIZE);
    else
        package_store_read(0, rsp + 5, PKG_HEADER_SIZE);
    *rsp_len = 5 + PKG_HEADER_SIZE;
    return PDK_OK;
}

uint16_t pkg_cmd_read(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 6)
        return PDK_BAD_PAYLOAD;
    if (package_store_state() == PDK_PKG_EMPTY)
        return PDK_STORE_EMPTY;
    uint32_t off = le32(req);
    uint16_t n = le16(req + 4);
    uint32_t size = package_store_size();
    if (n > PDK_MAX_PAYLOAD || off > size || n > size - off)
        return PDK_BAD_PAYLOAD;
    package_store_read(off, rsp, n);
    *rsp_len = n;
    return PDK_OK;
}

uint16_t pkg_cmd_delete(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (package_store_flags() & PDK_FLAG_MASTER_RESTORE_PENDING)
        return PDK_MASTER_RESTORE_PENDING;
    if (!package_store_delete())
        return PDK_STORE_WRITE_FAILED;
    pkg_send_state_event();
    return PDK_OK;
}

uint16_t pkg_cmd_check(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    uint32_t computed = 0;
    uint8_t before = package_store_state();
    rsp[0] = package_store_recheck(&computed);
    put32(rsp + 1, computed);
    *rsp_len = 5;
    if (rsp[0] != before)
        pkg_send_state_event();
    return PDK_OK;
}
