/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STM32G474 (WeAct core board) firmware: supplies off first, then clocks, timers, flash, supplies,
 * UI, USB, core. */
#include "g474.h"
#include "pdk_core.h"
#include "tusb.h"

int main(void)
{
    g474_supplies_early_off();   /* again, in case main() is entered without a reset */
    g474_clock_init();
    g474_time_init();
    g474_flash_init();
    g474_power_init();
    g474_ui_init();
    platform_usb_init();
    pdk_core_init();

    for (;;) {
        tud_task();
        pdk_core_poll();
        g474_ui_poll();
    }
}
