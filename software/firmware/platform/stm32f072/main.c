/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STM32F072 bring-up firmware: supplies off first, then clocks, timers, power front end, USB, core. */
#include "f072.h"
#include "pdk_core.h"
#include "tusb.h"

int main(void)
{
    f072_power_early_off();   /* again, in case main() is entered without a reset */
    f072_clock_init();
    f072_time_init();
    f072_power_init();
    f072_ui_init();
    f072_usb_init();
    pdk_core_init();

    for (;;) {
        tud_task();
        pdk_core_poll();
        f072_ui_poll();
    }
}
