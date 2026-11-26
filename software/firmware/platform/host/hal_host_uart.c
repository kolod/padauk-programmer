/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UART bridge of pdk-sim: the chip in the socket runs a "hello + echo" program — after power-up it sends
 * 'U' (the speed, 115200 baud, not passed on) and "hello\n", then echoes every byte it receives. Bytes
 * sent to the chip are also printed on stderr ("uart tx: …"). */
#include <stdio.h>
#include <string.h>

#include "hal_uart.h"
#include "host.h"

#define RING 512u
#define MODEL_BAUD 115200u

static struct {
    bool on;
    uint32_t baud;
    uint8_t ring[RING];
    size_t head, tail;
} u;

static void rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t next = (u.head + 1u) % RING;
        if (next == u.tail)
            return;   /* overrun: the rest is lost, as on a real UART */
        u.ring[u.head] = data[i];
        u.head = next;
    }
}

void hal_uart_start(void)
{
    memset(&u, 0, sizeof u);
    u.on = true;
    u.baud = MODEL_BAUD;   /* the model's 'U' has arrived and was dropped */
    rx((const uint8_t *)"hello\n", 6);
}

void hal_uart_stop(void) { u.on = false; }

uint32_t hal_uart_baud(void) { return u.on ? u.baud : 0; }

size_t hal_uart_read(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap && u.tail != u.head) {
        buf[n++] = u.ring[u.tail];
        u.tail = (u.tail + 1u) % RING;
    }
    if (!n)
        host_sleep_us(200);   /* the RUN loop polls this: do not spin the host CPU */
    return n;
}

bool hal_uart_write(const uint8_t *data, size_t len)
{
    if (!u.on || !u.baud)
        return false;
    fprintf(stderr, "uart tx: %.*s\n", (int)len, (const char *)data);
    fflush(stderr);
    rx(data, len);   /* the chip echoes */
    return true;
}

uint16_t hal_uart_vdd_max_mV(void) { return 0; }
