/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compile-time configuration of the programmer core. */
#ifndef PDK_CONFIG_H
#define PDK_CONFIG_H

#define PDK_FW_VERSION "0.1.0"

/* Largest frame payload this firmware accepts and sends (protocol limit: 1024). */
#ifndef PDK_MAX_PAYLOAD
#define PDK_MAX_PAYLOAD 512u
#endif

/* Decoded frame = 10-byte header + payload + 4-byte CRC. */
#define PDK_FRAME_OVERHEAD 14u
#define PDK_MAX_FRAME (PDK_FRAME_OVERHEAD + PDK_MAX_PAYLOAD)
/* COBS adds at most one byte per 254 plus one, plus the 0x00 delimiter. */
#define PDK_MAX_ENCODED (PDK_MAX_FRAME + PDK_MAX_FRAME / 254u + 2u)

/* Largest number of words a READ may return (largest supported chip). */
#define PDK_MAX_READ_WORDS 4096u

#endif
