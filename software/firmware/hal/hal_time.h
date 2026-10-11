/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PDK_HAL_TIME_H
#define PDK_HAL_TIME_H

#include <stdint.h>

uint32_t hal_time_ms(void);           /* monotonic milliseconds (wraps) */
uint32_t hal_time_us(void);           /* monotonic microseconds (wraps) */
void hal_delay_us(uint32_t us);       /* busy wait */

#endif
