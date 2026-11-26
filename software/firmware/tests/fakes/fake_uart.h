/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fake UART bridge (hal_uart.h) for unit tests; reset by fake_reset(). */
#ifndef PDK_FAKE_UART_H
#define PDK_FAKE_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void fake_uart_reset(void);
void fake_uart_baud(uint32_t baud);                       /* the speed the chip's 'U' gives (0: none yet) */
void fake_uart_chip_sends(const void *data, size_t len);  /* bytes from the chip, read once the speed is known */
size_t fake_uart_take_tx(uint8_t *buf, size_t cap);       /* bytes the programmer sent to the chip */
void fake_uart_tx_room(size_t room);                      /* TX buffer size (to test a full buffer) */
void fake_uart_hook(void (*fn)(void));                    /* called on every hal_uart_read() (each RUN loop pass) */
bool fake_uart_on(void);
unsigned fake_uart_starts(void);
unsigned fake_uart_stops(void);

#endif
