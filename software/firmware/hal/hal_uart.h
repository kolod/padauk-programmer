/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UART bridge to the chip in RUN (contracts/protocol.md §4 RUN, EVT_UART_LINK / EVT_UART_RX, UART_TX).
 * The chip talks first: its first byte 'U' (0x55) gives the speed by the length of its start bit and is
 * not passed on; until then nothing is received or sent. Received bytes go into a ring buffer that the
 * RUN loop drains. */
#ifndef PDK_HAL_UART_H
#define PDK_HAL_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The chip's UART lines become the bridge (called with VDD on); speed detection armed. */
void hal_uart_start(void);

/* The lines go back to Hi-Z (before the voltages are removed). */
void hal_uart_stop(void);

/* The detected speed in baud; 0 until the chip's 'U' arrived. */
uint32_t hal_uart_baud(void);

/* Takes up to `cap` received bytes; returns how many. */
size_t hal_uart_read(uint8_t *buf, size_t cap);

/* Queues `len` bytes for the chip: all of them, or none (false: no speed yet, or no room). */
bool hal_uart_write(const uint8_t *data, size_t len);

/* The highest chip VDD the bridge lines tolerate (0: no limit beyond the supply's). */
uint16_t hal_uart_vdd_max_mV(void);

#endif
