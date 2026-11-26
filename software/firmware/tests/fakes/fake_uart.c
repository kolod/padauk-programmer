/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fake UART bridge for unit tests: the test plays the chip (fake_uart_chip_sends, fake_uart_baud) and
 * reads what the programmer sent (fake_uart_take_tx). */
#include <string.h>

#include "fake_uart.h"
#include "hal_uart.h"

static struct {
    bool on;
    uint32_t baud, detected;
    uint8_t rx[1024];
    size_t rx_len, rx_pos;
    uint8_t tx[1024];
    size_t tx_len;
    size_t tx_room;
    unsigned starts, stops;
    void (*hook)(void);
} u;

void fake_uart_reset(void)
{
    memset(&u, 0, sizeof u);
    u.tx_room = sizeof u.tx;
}

void fake_uart_baud(uint32_t baud) { u.baud = baud; }

void fake_uart_chip_sends(const void *data, size_t len)
{
    if (u.rx_pos == u.rx_len)
        u.rx_pos = u.rx_len = 0;
    memcpy(u.rx + u.rx_len, data, len);
    u.rx_len += len;
}

size_t fake_uart_take_tx(uint8_t *buf, size_t cap)
{
    size_t n = u.tx_len < cap ? u.tx_len : cap;
    memcpy(buf, u.tx, n);
    memmove(u.tx, u.tx + n, u.tx_len - n);
    u.tx_len -= n;
    return n;
}

void fake_uart_hook(void (*fn)(void)) { u.hook = fn; }
void fake_uart_tx_room(size_t room) { u.tx_room = room; }
bool fake_uart_on(void) { return u.on; }
unsigned fake_uart_starts(void) { return u.starts; }
unsigned fake_uart_stops(void) { return u.stops; }

void hal_uart_start(void)
{
    u.on = true;
    u.starts++;
}

void hal_uart_stop(void)
{
    u.on = false;
    u.stops++;
}

uint32_t hal_uart_baud(void) { return u.on ? u.baud : 0; }

size_t hal_uart_read(uint8_t *buf, size_t cap)
{
    if (u.hook)
        u.hook();
    if (!u.on || !u.baud)
        return 0;
    size_t n = u.rx_len - u.rx_pos < cap ? u.rx_len - u.rx_pos : cap;
    memcpy(buf, u.rx + u.rx_pos, n);
    u.rx_pos += n;
    return n;
}

bool hal_uart_write(const uint8_t *data, size_t len)
{
    if (!u.on || !u.baud || u.tx_len + len > u.tx_room)
        return false;
    memcpy(u.tx + u.tx_len, data, len);
    u.tx_len += len;
    return true;
}

uint16_t hal_uart_vdd_max_mV(void) { return 0; }
