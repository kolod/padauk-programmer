/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Raw entropy for serial numbers (research R18). */
#ifndef PDK_HAL_ENTROPY_H
#define PDK_HAL_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fills `buf` with raw noise bits (hardware RNG or ADC noise). Returns false if the source failed. */
bool hal_entropy_raw(uint8_t *buf, size_t len);

/* 96-bit unique ID of the programmer MCU. */
void hal_uid(uint8_t uid[12]);

#endif
