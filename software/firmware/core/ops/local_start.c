/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local actions without a PC (FR-054, research R16): pdk_local_start(request) becomes an operation with
 * source = local, by priority — SELF_TEST not passed → SELF_TEST; MASTER_RESTORE_PENDING →
 * RESTORE_MASTER; otherwise the requested one. A request while an operation runs is ignored;
 * pdk_local_cancel() cancels only a local operation. */
#include "hal_ui.h"
#include "indication.h"
#include "op_manager.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"

void pdk_local_start(pdk_local_request_t request)
{
    if (op_manager_busy())
        return;
    bool restore_pending = (package_store_flags() & PDK_FLAG_MASTER_RESTORE_PENDING) != 0;
    op_request_t req = {.source = PDK_SOURCE_LOCAL};
    if (op_selftest_state() != PDK_SELFTEST_PASSED) {
        req.op_type = PDK_OP_SELF_TEST;
    } else if (restore_pending) {
        req.op_type = PDK_OP_RESTORE_MASTER;
    } else {
        switch (request) {
        case PDK_LOCAL_PROGRAM: req.op_type = PDK_OP_PROGRAM; break;   /* PKG_CHECK refuses EMPTY / CORRUPTED */
        case PDK_LOCAL_COPY: req.op_type = PDK_OP_COPY; break;
        case PDK_LOCAL_VERIFY: req.op_type = PDK_OP_VERIFY; break;
        case PDK_LOCAL_SELF_TEST: req.op_type = PDK_OP_SELF_TEST; break;
        case PDK_LOCAL_RESTORE_MASTER: return;   /* only when a restore is pending */
        default: return;
        }
    }
    uint16_t id;
    uint16_t st = op_manager_start(&req, &id);
    if (st != PDK_OK)
        indication_fail(st);   /* e.g. COPY / RESTORE_MASTER not available in this build */
}

void pdk_local_cancel(void)
{
    op_status_t s;
    op_manager_status(&s);
    if (s.running && op_request()->source == PDK_SOURCE_LOCAL)
        op_manager_cancel(s.op_id);
}
