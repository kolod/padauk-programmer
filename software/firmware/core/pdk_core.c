/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_core.h"

#include "package/upload.h"
#include "power/power.h"
#include "proto/session.h"
#include "storage/housekeeping.h"
#include "storage/package_store.h"

void pdk_core_init(void)
{
    power_off_all();          /* before anything else: no voltage on the chip after reset */
    package_store_init();
    housekeeping_init();
    upload_init();
    session_init();
}

void pdk_core_poll(void)
{
    session_poll();
    housekeeping_poll();   /* background erase, one step per loop */
}

void pdk_core_on_session_reset(void)
{
    /* An unfinished upload is discarded; operations with the chip are NOT interrupted (FR-023). */
    upload_abort();
}
