/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dispatch.h"

#include <stddef.h>
#include <string.h>

#include "chips/chip_table.h"
#include "hal_flash.h"
#include "hal_platform.h"
#include "le.h"
#include "ops/op_manager.h"
#include "package/pkg_cmds.h"
#include "pdk_config.h"
#include "proto_defs.h"
#include "session.h"

static uint16_t cmd_hello(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);
static uint16_t cmd_get_info(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

static const pdk_command_t commands[] = {
    {PDK_CMD_HELLO, PDK_CMDF_NO_SESSION | PDK_CMDF_DURING_OP, cmd_hello},
    {PDK_CMD_GET_INFO, PDK_CMDF_NO_SESSION | PDK_CMDF_DURING_OP, cmd_get_info},
    {PDK_CMD_LIST_CHIPS, 0, chip_cmd_list},
    {PDK_CMD_PKG_BEGIN, 0, pkg_cmd_begin},
    {PDK_CMD_PKG_DATA, 0, pkg_cmd_data},
    {PDK_CMD_PKG_COMMIT, 0, pkg_cmd_commit},
    {PDK_CMD_PKG_ABORT, 0, pkg_cmd_abort},
    {PDK_CMD_PKG_INFO, PDK_CMDF_DURING_OP, pkg_cmd_info},
    {PDK_CMD_PKG_READ, 0, pkg_cmd_read},
    {PDK_CMD_PKG_DELETE, 0, pkg_cmd_delete},
    {PDK_CMD_PKG_CHECK, 0, pkg_cmd_check},
};

static uint32_t capabilities(void)
{
    /* Bits are set as the corresponding features are implemented. */
    return PDK_CAP_MEASURE_POWER;
}

uint16_t dispatch_build_info(uint8_t *out)
{
    const hal_flash_geometry_t *g = hal_flash_geometry();
    uint32_t max_read = g->slot_size / 2u;
    if (max_read > PDK_MAX_READ_WORDS)
        max_read = PDK_MAX_READ_WORDS;
    out[0] = PDK_PROTO_MAJOR;
    out[1] = PDK_PROTO_MINOR;
    put16(out + 2, PDK_MAX_PAYLOAD);
    put32(out + 4, g->slot_size);
    put16(out + 8, (uint16_t)max_read);
    put32(out + 10, capabilities());
    put_str(out + 14, 16, hal_platform_name());
    put_str(out + 30, 24, PDK_FW_VERSION);
    out[54] = (uint8_t)chip_count();
    return 55;
}

static uint16_t cmd_get_info(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    *rsp_len = dispatch_build_info(rsp);
    return PDK_OK;
}

static uint16_t cmd_hello(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len < 2 || len > 2 + 32)
        return PDK_BAD_PAYLOAD;
    *rsp_len = dispatch_build_info(rsp);
    if (req[0] != PDK_PROTO_MAJOR) {
        session_set_established(false);
        return PDK_PROTO_VERSION;
    }
    session_set_established(true);
    return PDK_OK;
}

uint16_t dispatch_request(uint16_t code, const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    *rsp_len = 0;
    const pdk_command_t *cmd = NULL;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        if (commands[i].code == code) {
            cmd = &commands[i];
            break;
        }
    }
    if (!cmd)
        return PDK_UNKNOWN_COMMAND;
    if (!(cmd->flags & PDK_CMDF_NO_SESSION) && !session_is_established())
        return PDK_NO_SESSION;
    if (!(cmd->flags & PDK_CMDF_DURING_OP) && op_manager_busy())
        return PDK_BUSY;
    return cmd->fn(req, len, rsp, rsp_len);
}
