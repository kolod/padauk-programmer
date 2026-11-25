/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_core.h"

#include "hal_ui.h"
#include "ops/op_manager.h"
#include "package/upload.h"
#include "power/power.h"
#include "power/supply_cal.h"
#include "power/supply_set.h"
#include "proto/proto_defs.h"
#include "proto/session.h"
#include "storage/housekeeping.h"
#include "storage/package_store.h"

void pdk_core_init(void)
{
    power_off_all();          /* before anything else: no voltage on the chip after reset */
    package_store_init();
    housekeeping_init();
    supply_cal_init();   /* stored supply calibration → power layer */
    supply_set_init();
    upload_init();
    op_manager_init();
    session_init();
    op_request_t selftest = {.op_type = PDK_OP_SELF_TEST, .source = PDK_SOURCE_LOCAL};
    uint16_t id;
    op_manager_start(&selftest, &id);   /* runs on the first pdk_core_poll() (FR-041) */
}

void pdk_core_poll(void)
{
    session_poll();
    housekeeping_poll();   /* background erase, one step per loop */
    supply_cal_poll();
    supply_set_poll();
    hal_ui_poll();
    op_manager_poll();   /* a started operation runs to the end here */
}

void pdk_core_on_session_reset(void)
{
    /* An unfinished upload is discarded; operations with the chip are NOT interrupted (FR-023). */
    upload_abort();
    supply_cal_abort();   /* supplies raised for calibration or by SUPPLY_SET are not left on without a host */
    supply_set_abort();
}
