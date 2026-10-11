/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LIST_CHIPS: u8 first_index → u8 total, u8 count, CHIP_DESC[count] (contracts/protocol.md §4). */
#include "chip_table.h"
#include "pdk_config.h"
#include "proto/proto_defs.h"

uint16_t chip_cmd_list(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 1)
        return PDK_BAD_PAYLOAD;
    size_t first = req[0];
    size_t total = chip_count();
    size_t per_frame = (PDK_MAX_PAYLOAD - 2u) / CHIP_DESC_SIZE;
    size_t count = 0;
    for (size_t i = first; i < total && count < per_frame; i++, count++)
        chip_encode_desc(chip_at(i), (uint8_t)i, rsp + 2 + count * CHIP_DESC_SIZE);
    rsp[0] = (uint8_t)total;
    rsp[1] = (uint8_t)count;
    *rsp_len = (uint16_t)(2 + count * CHIP_DESC_SIZE);
    return PDK_OK;
}
