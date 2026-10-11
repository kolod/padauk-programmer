/* SPDX-License-Identifier: GPL-3.0-or-later */
/* COBS framing: every frame on the wire is COBS-encoded and terminated by 0x00. */
#ifndef PDK_COBS_H
#define PDK_COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Encodes `len` bytes into `out` (capacity `cap`) and appends the 0x00 delimiter.
 * Returns the number of bytes written, or 0 if `cap` is too small. */
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Decodes one COBS block (without the delimiter). Returns decoded length or -1 on error. */
int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Streaming receiver: collects bytes up to each 0x00 and decodes them. */
typedef struct {
    uint8_t *buf;     /* raw (encoded) bytes of the current frame */
    size_t cap;
    size_t len;
    bool overflow;    /* frame too long: discard until next delimiter */
} cobs_rx_t;

typedef enum {
    COBS_RX_NONE = 0,   /* no complete frame yet */
    COBS_RX_FRAME,      /* `out`/`out_len` hold a decoded frame */
    COBS_RX_ERROR,      /* a frame ended but was malformed or too long */
} cobs_rx_result_t;

void cobs_rx_init(cobs_rx_t *rx, uint8_t *buf, size_t cap);

/* Feeds one byte. On a delimiter decodes the collected block into `out`.
 * Empty blocks (consecutive delimiters) are ignored (COBS_RX_NONE). */
cobs_rx_result_t cobs_rx_byte(cobs_rx_t *rx, uint8_t byte, uint8_t *out, size_t out_cap, size_t *out_len);

#endif
