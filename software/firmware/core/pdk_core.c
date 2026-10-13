/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_core.h"

#include "package/upload.h"
#include "power/power.h"
#include "power/supply_cal.h"
#include "proto/session.h"
#include "storage/housekeeping.h"
#include "storage/package_store.h"

void pdk_core_init(void)
{
    power_off_all();          /* before anything else: no voltage on the chip after reset */
    package_store_init();
    housekeeping_init();
    supply_cal_init();   /* stored supply calibration → power layer */
    upload_init();
    session_init();
}

void pdk_core_poll(void)
{
    session_poll();
    housekeeping_poll();   /* background erase, one step per loop */
    supply_cal_poll();
}

void pdk_core_on_session_reset(void)
{
    /* An unfinished upload is discarded; operations with the chip are NOT interrupted (FR-023). */
    upload_abort();
    supply_cal_abort();   /* supplies raised for calibration are not left on without a host */
}
