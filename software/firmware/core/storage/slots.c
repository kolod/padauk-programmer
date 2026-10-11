/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "slots.h"

#include "hal_flash.h"
#include "housekeeping.h"
#include "proto/crc32.h"

static uint32_t clean[HAL_FLASH_MAX_SLOTS];   /* bit p = page p of the slot is erased and unwritten */
static uint32_t ready;                        /* upload slot: page erased during this upload (may hold its data) */
static uint8_t active = SLOT_NONE, upload = SLOT_NONE;

static const hal_flash_geometry_t *geo(void) { return hal_flash_geometry(); }
static uint32_t pages(void) { return geo()->slot_size / geo()->page_size; }
static uint32_t all_pages(void) { return pages() >= 32u ? 0xFFFFFFFFu : (1u << pages()) - 1u; }

uint32_t slots_size(void) { return geo()->slot_size; }
uint8_t slots_count(void) { return geo()->slot_count; }

bool flash_page_blank(uint32_t addr)
{
    uint8_t buf[64];
    for (uint32_t off = 0; off < geo()->page_size; off += sizeof buf) {
        hal_flash_read(addr + off, buf, sizeof buf);
        for (unsigned i = 0; i < sizeof buf; i++)
            if (buf[i] != 0xFF)
                return false;
    }
    return true;
}

void slots_init(uint8_t active_slot)
{
    active = active_slot;
    upload = SLOT_NONE;
    for (uint8_t s = 0; s < slots_count(); s++) {
        clean[s] = 0;
        if (s == active)
            continue;
        for (uint32_t p = 0; p < pages(); p++)
            if (flash_page_blank(geo()->slot_addr[s] + p * geo()->page_size))
                clean[s] |= 1u << p;
    }
}

void slots_set_active(uint8_t slot)
{
    if (slot < slots_count())
        clean[slot] = 0;   /* holds data now */
    active = slot;
}

void slots_set_upload(uint8_t slot)
{
    upload = slot;
    ready = slot < slots_count() ? clean[slot] : 0;
}

bool slots_is_clean(uint8_t slot) { return slot < slots_count() && clean[slot] == all_pages(); }

uint8_t slots_pick_upload(void)
{
    uint8_t fallback = SLOT_NONE;
    for (uint8_t s = 0; s < slots_count(); s++) {
        if (s == active)
            continue;
        if (slots_is_clean(s))
            return s;
        if (fallback == SLOT_NONE)
            fallback = s;
    }
    return fallback;
}

void slots_read(uint8_t slot, uint32_t offset, uint8_t *buf, size_t len)
{
    hal_flash_read(geo()->slot_addr[slot % slots_count()] + offset, buf, len);
}

bool slots_write(uint8_t slot, uint32_t offset, const uint8_t *data, size_t len)
{
    const hal_flash_geometry_t *g = geo();
    if (slot >= g->slot_count || (offset | len) & 1u || offset + len > g->slot_size)
        return false;
    if (!len)
        return true;
    uint32_t base = g->slot_addr[slot];
    uint32_t first = offset / g->page_size;
    uint32_t last = (offset + (uint32_t)len - 1u) / g->page_size;
    if (slot != upload)
        slots_set_upload(slot);   /* a write always belongs to the running upload */
    for (uint32_t p = first; p <= last; p++) {
        if (ready & (1u << p))
            continue;
        housekeeping_finish();   /* the page may be erasing in the background right now */
        if (ready & (1u << p))
            continue;
        if (!hal_flash_erase_page(base + p * g->page_size))
            return false;
        ready |= 1u << p;
    }
    for (uint32_t p = first; p <= last; p++)
        clean[slot] &= ~(1u << p);   /* dirty from the first byte written */
    return hal_flash_program(base + offset, data, len);
}

uint32_t slots_crc(uint8_t slot, uint32_t len)
{
    uint8_t buf[64];
    uint32_t crc = crc32_init();
    for (uint32_t off = 0; off < len;) {
        uint32_t n = len - off < sizeof buf ? len - off : (uint32_t)sizeof buf;
        slots_read(slot, off, buf, n);
        crc = crc32_update(crc, buf, n);
        off += n;
    }
    return crc32_final(crc);
}

bool slots_next_dirty(uint32_t *addr)
{
    for (uint8_t s = 0; s < slots_count(); s++) {
        if (s == active || s == upload || clean[s] == all_pages())
            continue;
        for (uint32_t p = 0; p < pages(); p++) {
            if (!(clean[s] & (1u << p))) {
                *addr = geo()->slot_addr[s] + p * geo()->page_size;
                return true;
            }
        }
    }
    return false;
}

void slots_page_erased(uint32_t addr, bool ok)
{
    for (uint8_t s = 0; s < slots_count(); s++) {
        uint32_t base = geo()->slot_addr[s];
        if (addr < base || addr >= base + geo()->slot_size)
            continue;
        /* An upload may have picked this slot meanwhile: it is still fine, slots_write() waits for this
         * erase before touching the page. A slot that became active stays dirty. */
        if (ok && s != active && flash_page_blank(addr)) {
            uint32_t bit = 1u << ((addr - base) / geo()->page_size);
            clean[s] |= bit;
            if (s == upload)
                ready |= bit;
        }
        return;
    }
}
