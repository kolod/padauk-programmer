/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Package slots in flash (research R5). Every page of every slot is tracked as clean (erased) or
 * dirty; dirty pages of slots that hold nothing useful are erased in the background by
 * housekeeping, so an upload normally writes into clean pages without erasing anything. */
#ifndef PDK_SLOTS_H
#define PDK_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_NONE 0xFFu

uint32_t slots_size(void);
uint8_t slots_count(void);

/* Blank-checks every slot except `active` (whose pages count as dirty). */
void slots_init(uint8_t active);

/* Slots never erased in the background: the active package and the current upload target. */
void slots_set_active(uint8_t slot);
void slots_set_upload(uint8_t slot);   /* SLOT_NONE when no upload is running */

/* Upload target: a fully clean slot other than the active one if any, else the first other slot. */
uint8_t slots_pick_upload(void);
bool slots_is_clean(uint8_t slot);

void slots_read(uint8_t slot, uint32_t offset, uint8_t *buf, size_t len);

/* Writes `len` bytes at `offset` (even). A page that is not clean yet is erased first (waiting for
 * a background erase of that page if one is running). Written pages become dirty. */
bool slots_write(uint8_t slot, uint32_t offset, const uint8_t *data, size_t len);

/* CRC-32 of the first `len` bytes of the slot. */
uint32_t slots_crc(uint8_t slot, uint32_t len);

/* Housekeeping interface: next dirty page that may be erased now; result of its erase. */
bool slots_next_dirty(uint32_t *addr);
void slots_page_erased(uint32_t addr, bool ok);

/* True if the whole page at `addr` reads 0xFF. */
bool flash_page_blank(uint32_t addr);

#endif
