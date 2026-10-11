/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB CDC transport through TinyUSB; the port counts as open while the host asserts DTR. */
#include "f072.h"
#include "hal_transport.h"
#include "tusb.h"

void USB_IRQHandler(void) { tud_int_handler(0); }

void f072_usb_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
    tud_init(0);
}

size_t hal_transport_read(uint8_t *buf, size_t max)
{
    tud_task();
    if (!tud_cdc_available())
        return 0;
    return tud_cdc_read(buf, (uint32_t)max);
}

void hal_transport_write(const uint8_t *buf, size_t len)
{
    while (len && tud_cdc_connected()) {
        uint32_t n = tud_cdc_write(buf, (uint32_t)len);
        buf += n;
        len -= n;
        if (len) {
            tud_cdc_write_flush();
            tud_task();
        }
    }
    tud_cdc_write_flush();
}

bool hal_transport_is_open(void)
{
    tud_task();
    return tud_cdc_connected();
}
