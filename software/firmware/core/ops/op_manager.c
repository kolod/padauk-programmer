/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "op_manager.h"

#include "hal_ui.h"

bool op_manager_busy(void)
{
    return false;
}

/* Local actions (FR-054) are implemented with the operations (US3). Until then a local request is
 * reported as "blocked" so the operator sees that nothing is executed. */
void pdk_local_start(pdk_local_request_t request)
{
    (void)request;
    pdk_ui_detail_t d = {0};
    hal_ui_state(PDK_UI_BLOCKED, &d);
}

void pdk_local_cancel(void)
{
}
