/* SPDX-License-Identifier: GPL-3.0-or-later */
/* State journal: 40-byte records with their own CRC in two flash pages (data-model StateRecord, R6). */
#ifndef PDK_JOURNAL_H
#define PDK_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#define JOURNAL_RECORD_SIZE 40u
#define JOURNAL_NO_SLOT 0xFFu

typedef struct {
    uint32_t seq;
    uint8_t active_slot;      /* 0, 1 or JOURNAL_NO_SLOT */
    uint8_t cfg_copy_ihrc;
    uint8_t cfg_copy_ilrc;
    uint8_t cfg_copy_bg;
    uint16_t flags;           /* PDK_FLAG_MASTER_RESTORE_PENDING */
    uint32_t package_size;
    uint32_t package_crc;
    uint32_t ok_count;
    uint32_t fail_count;
} journal_state_t;

void journal_record_encode(const journal_state_t *st, uint8_t out[JOURNAL_RECORD_SIZE]);
bool journal_record_decode(const uint8_t in[JOURNAL_RECORD_SIZE], journal_state_t *st);

/* Scans both pages; the current state is the valid record with the highest seq (defaults if none). */
void journal_init(void);
const journal_state_t *journal_get(void);

/* Appends `st` as a new record (seq = current + 1). Switches pages when the current one is full;
 * the spare page is normally erased in the background already (housekeeping). */
bool journal_append(const journal_state_t *st);

/* Housekeeping: the spare page (the one not holding the current record) and its erase result. */
bool journal_spare_dirty(uint32_t *addr);
void journal_spare_erased(uint32_t addr, bool ok);

#endif
