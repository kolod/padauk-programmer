/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hooks every MCU platform provides to the shared code in platform/common/. */
#ifndef PDK_PLATFORM_COMMON_H
#define PDK_PLATFORM_COMMON_H

/* All lines to the PADAUK chip as inputs (Hi-Z). Used by the chip engine safe state. */
void platform_ic_lines_hiz(void);

/* Enables the USB peripheral clock and pins, then calls tud_init(). */
void platform_usb_init(void);

#endif
