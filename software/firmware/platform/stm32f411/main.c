/* SPDX-License-Identifier: GPL-3.0-or-later */
/* STM32F411 (WeAct Black Pill + W25Q64) firmware: clocks, timers, storage, USB, core. */
#include "f411.h"
#include "pdk_core.h"
#include "tusb.h"

int main(void)
{
    f411_clock_init();   /* no supplies to switch off first: this board has no front end yet */
    f411_time_init();
    f411_adc_init();
    f411_ui_init();
    w25q_init();   /* without the flash every storage operation fails: STORE_WRITE_FAILED */
    platform_usb_init();
    pdk_core_init();

    for (;;) {
        tud_task();
        pdk_core_poll();
        f411_ui_poll();
    }
}
