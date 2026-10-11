/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "journal.h"

#include <string.h>

#include "hal_flash.h"
#include "proto/crc32.h"
#include "proto/le.h"

static journal_state_t current;
static uint8_t cur_page;       /* page of the latest record */
static uint32_t next_index;    /* first free record index in cur_page */

static uint32_t records_per_page(void) { return hal_flash_geometry()->page_size / JOURNAL_RECORD_SIZE; }

static uint32_t record_addr(uint8_t page, uint32_t index)
{
    return hal_flash_geometry()->journal_addr[page & 1u] + index * JOURNAL_RECORD_SIZE;
}

void journal_record_encode(const journal_state_t *st, uint8_t out[JOURNAL_RECORD_SIZE])
{
    memcpy(out, "PDKS", 4);
    put32(out + 4, st->seq);
    out[8] = st->active_slot;
    out[9] = st->cfg_copy_ihrc;
    put16(out + 10, st->flags);
    put32(out + 12, st->package_size);
    put32(out + 16, st->package_crc);
    put32(out + 20, 0xFFFFFFFFu);          /* reserved0 */
    put32(out + 24, st->ok_count);
    put32(out + 28, st->fail_count);
    out[32] = st->cfg_copy_ilrc;
    out[33] = st->cfg_copy_bg;
    put16(out + 34, 0xFFFFu);              /* reserved */
    put32(out + 36, crc32_compute(out, 36));
}

bool journal_record_decode(const uint8_t in[JOURNAL_RECORD_SIZE], journal_state_t *st)
{
    if (memcmp(in, "PDKS", 4) != 0 || crc32_compute(in, 36) != le32(in + 36))
        return false;
    st->seq = le32(in + 4);
    st->active_slot = in[8];
    st->cfg_copy_ihrc = in[9];
    st->flags = le16(in + 10);
    st->package_size = le32(in + 12);
    st->package_crc = le32(in + 16);
    st->ok_count = le32(in + 24);
    st->fail_count = le32(in + 28);
    st->cfg_copy_ilrc = in[32];
    st->cfg_copy_bg = in[33];
    return true;
}

static bool is_erased(const uint8_t *b)
{
    for (unsigned i = 0; i < JOURNAL_RECORD_SIZE; i++)
        if (b[i] != 0xFF)
            return false;
    return true;
}

void journal_init(void)
{
    memset(&current, 0, sizeof current);
    current.active_slot = JOURNAL_NO_SLOT;
    current.cfg_copy_bg = 1;
    bool found = false;
    uint32_t used[2] = {0, 0};   /* index after the last non-erased record per page */
    uint8_t rec[JOURNAL_RECORD_SIZE];

    for (uint8_t page = 0; page < 2; page++) {
        for (uint32_t i = 0; i < records_per_page(); i++) {
            hal_flash_read(record_addr(page, i), rec, sizeof rec);
            if (is_erased(rec))
                continue;
            used[page] = i + 1;
            journal_state_t st;
            if (journal_record_decode(rec, &st) && (!found || (int32_t)(st.seq - current.seq) > 0)) {
                current = st;
                cur_page = page;
                found = true;
            }
        }
    }
    if (!found)
        cur_page = 0;
    next_index = used[cur_page];
}

const journal_state_t *journal_get(void) { return &current; }

bool journal_append(const journal_state_t *st)
{
    const hal_flash_geometry_t *g = hal_flash_geometry();
    journal_state_t rec_st = *st;
    rec_st.seq = current.seq + 1u;

    if (next_index >= records_per_page()) {
        uint8_t other = (uint8_t)(cur_page ^ 1u);
        if (!hal_flash_erase_page(g->journal_addr[other]))
            return false;
        cur_page = other;
        next_index = 0;
    }
    uint8_t rec[JOURNAL_RECORD_SIZE];
    journal_record_encode(&rec_st, rec);
    uint32_t addr = record_addr(cur_page, next_index);
    next_index++;   /* even a failed write consumes the slot */
    if (!hal_flash_program(addr, rec, sizeof rec))
        return false;
    uint8_t back[JOURNAL_RECORD_SIZE];
    hal_flash_read(addr, back, sizeof back);
    if (memcmp(back, rec, sizeof rec) != 0)
        return false;
    current = rec_st;
    return true;
}
