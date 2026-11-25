/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config_cmds.h"

#include "journal.h"
#include "proto/proto_defs.h"

uint16_t config_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    const journal_state_t *j = journal_get();
    rsp[0] = j->cfg_copy_ihrc;
    rsp[1] = j->cfg_copy_ilrc;
    rsp[2] = j->cfg_copy_bg;
    *rsp_len = 3;
    return PDK_OK;
}

/* Allowed locally: IHRC KEEP_VALUE (0) or SNAP_STANDARD (2), ILRC KEEP_VALUE, BandGap keep (0) or restore (1). */
uint16_t config_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 3 || (req[0] != 0 && req[0] != 2) || req[1] != 0 || req[2] > 1)
        return PDK_BAD_PAYLOAD;
    journal_state_t st = *journal_get();
    if (st.cfg_copy_ihrc == req[0] && st.cfg_copy_ilrc == req[1] && st.cfg_copy_bg == req[2])
        return PDK_OK;
    st.cfg_copy_ihrc = req[0];
    st.cfg_copy_ilrc = req[1];
    st.cfg_copy_bg = req[2];
    return journal_append(&st) ? PDK_OK : PDK_STORE_WRITE_FAILED;
}
