/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB descriptors: CDC ACM, VID/PID 1209:0001 (pid.codes test PID; a dedicated PID is TBD). */
#include "tusb.h"

#include "hal_entropy.h"

#define USB_VID 0x1209
#define USB_PID 0x0001

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0101,   /* bump when descriptors change: Windows caches them per VID/PID/bcdDevice */
    .iManufacturer = 1,   /* STR_* below */
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&device; }

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_COUNT };
#define EP_NOTIF 0x81
#define EP_OUT 0x02
#define EP_IN 0x82
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0x80, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4 /* STR_CDC */, EP_NOTIF, 8, EP_OUT, EP_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration;
}

/* Indexed by string descriptor index: 1 manufacturer, 2 product, 3 serial (UID), 4 CDC interface.
 * Windows shows the interface string as "Bus reported device description" of the COM port. */
enum { STR_LANGID = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC };
static const char *const strings[] = {
    [STR_MANUFACTURER] = "padauk-programmer",
    [STR_PRODUCT] = "PADAUK Programmer",
    [STR_CDC] = "PADAUK Programmer",
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    uint8_t n;
    if (index == STR_LANGID) {
        buf[1] = 0x0409;
        n = 1;
    } else if (index == STR_SERIAL) {   /* MCU UID in hex */
        uint8_t uid[12];
        hal_uid(uid);
        static const char hex[] = "0123456789ABCDEF";
        for (n = 0; n < 24; n++)
            buf[1 + n] = (uint16_t)hex[(uid[n / 2] >> ((n & 1) ? 0 : 4)) & 0xF];
    } else if (index < sizeof strings / sizeof strings[0] && strings[index]) {
        const char *s = strings[index];
        for (n = 0; s[n] && n < 31; n++)
            buf[1 + n] = (uint16_t)s[n];
    } else {
        return NULL;
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
    return buf;
}
