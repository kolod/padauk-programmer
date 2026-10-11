/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Port session: COBS framing, stop-and-wait with response cache, NAK, events (contracts/protocol.md §2–§3). */
#ifndef PDK_SESSION_H
#define PDK_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void session_init(void);

/* Reads the transport, handles complete frames, tracks port open/close. Call from the main loop. */
void session_poll(void);

/* Feeds received bytes directly (used by session_poll and by tests). */
void session_rx_bytes(const uint8_t *data, size_t len);

bool session_is_open(void);
bool session_is_established(void);
void session_set_established(bool established);

/* Sends an unsolicited event frame (dropped while the port is closed). */
void session_send_event(uint16_t code, const uint8_t *payload, uint16_t len);

#endif
