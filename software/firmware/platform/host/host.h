/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Simulator-only configuration hooks for the host HAL. */
#ifndef PDK_HOST_H
#define PDK_HOST_H

#include <stdbool.h>
#include <stdint.h>

bool host_transport_listen(const char *host, uint16_t port);
void host_transport_shutdown(void);

bool host_flash_open(const char *path);       /* NULL: RAM only */
void host_flash_power_fail_after(long n);     /* -1: off */
void host_flash_corrupt_during_write(bool on);

/* "vdd:low", "vpp:slow", "vpp:stuck", ... */
bool host_power_fault(const char *spec);
bool host_power_scale(const char *spec);           /* "vdd|vpp:OUT:MEAS" scale errors */

void host_set_uid(const uint8_t uid[12]);
void host_sleep_us(uint32_t us);

#endif
