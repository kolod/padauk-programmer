/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STATS_GET / STATS_RESET: chips programmed ok / failed, kept in the state journal. */
#include "stats_cmds.h"

#include "journal.h"
#include "proto/le.h"
#include "proto/proto_defs.h"

uint16_t stats_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    const journal_state_t *j = journal_get();
    put32(rsp, j->ok_count);
    put32(rsp + 4, j->fail_count);
    *rsp_len = 8;
    return PDK_OK;
}

uint16_t stats_cmd_reset(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    journal_state_t st = *journal_get();
    if (!st.ok_count && !st.fail_count)
        return PDK_OK;   /* nothing to write */
    st.ok_count = st.fail_count = 0;
    return journal_append(&st) ? PDK_OK : PDK_STORE_WRITE_FAILED;
}
