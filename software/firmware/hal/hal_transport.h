/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Byte transport to the PC (USB CDC on hardware, TCP/stdio in the simulator). */
#ifndef PDK_HAL_TRANSPORT_H
#define PDK_HAL_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Non-blocking: copies up to `max` received bytes into `buf`, returns the count (0 if none). */
size_t hal_transport_read(uint8_t *buf, size_t max);

/* Queues `len` bytes for sending; blocks until they are accepted or the port closes. */
void hal_transport_write(const uint8_t *buf, size_t len);

/* True while the host has the port open (DTR asserted / TCP connected). */
bool hal_transport_is_open(void);

#endif
