/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "package_store.h"

#include <string.h>

#include "journal.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "slots.h"

static uint8_t state;

static uint8_t compute_state(uint32_t *computed)
{
    const journal_state_t *j = journal_get();
    if (computed)
        *computed = 0;
    if (j->active_slot >= slots_count())
        return PDK_PKG_EMPTY;
    if (j->package_size < 8 || j->package_size > slots_size())
        return PDK_PKG_CORRUPTED;
    uint32_t crc = slots_crc(j->active_slot, j->package_size - 4u);
    uint8_t trailer[4];
    slots_read(j->active_slot, j->package_size - 4u, trailer, 4);
    if (computed)
        *computed = crc;
    return (crc == j->package_crc && crc == le32(trailer)) ? PDK_PKG_VALID : PDK_PKG_CORRUPTED;
}

void package_store_init(void)
{
    journal_init();
    state = compute_state(NULL);
    slots_init(state == PDK_PKG_EMPTY ? SLOT_NONE : journal_get()->active_slot);
}

uint8_t package_store_state(void) { return state; }
uint32_t package_store_size(void) { return state == PDK_PKG_EMPTY ? 0 : journal_get()->package_size; }
uint32_t package_store_crc(void) { return state == PDK_PKG_EMPTY ? 0 : journal_get()->package_crc; }
uint8_t package_store_active_slot(void) { return journal_get()->active_slot; }
uint16_t package_store_flags(void) { return journal_get()->flags; }

uint8_t package_store_upload_slot(void) { return slots_pick_upload(); }

uint8_t package_store_recheck(uint32_t *computed_crc)
{
    state = compute_state(computed_crc);
    return state;
}

bool package_store_commit(uint8_t slot, uint32_t size, uint32_t crc)
{
    journal_state_t st = *journal_get();
    st.active_slot = slot;
    st.package_size = size;
    st.package_crc = crc;
    if (!journal_append(&st))
        return false;
    slots_set_active(slot);   /* the previous package's slot is erased in the background */
    state = compute_state(NULL);
    return state == PDK_PKG_VALID;
}

bool package_store_delete(void)
{
    journal_state_t st = *journal_get();
    st.active_slot = JOURNAL_NO_SLOT;
    st.package_size = 0;
    st.package_crc = 0;
    if (!journal_append(&st))
        return false;
    slots_set_active(SLOT_NONE);   /* the deleted package is erased in the background */
    state = PDK_PKG_EMPTY;
    return true;
}

bool package_store_save_supply_cal(const int8_t cal[4], bool valid)
{
    journal_state_t st = *journal_get();
    if (valid) {
        st.flags |= JOURNAL_FLAG_SUPPLY_CAL;
        memcpy(st.supply_cal, cal, sizeof st.supply_cal);
    } else {
        st.flags &= (uint16_t)~JOURNAL_FLAG_SUPPLY_CAL;
        memset(st.supply_cal, 0, sizeof st.supply_cal);
    }
    return journal_append(&st);
}

void package_store_read(uint32_t offset, uint8_t *buf, size_t len)
{
    slots_read(journal_get()->active_slot, offset, buf, len);
}
