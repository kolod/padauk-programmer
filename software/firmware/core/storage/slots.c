/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "slots.h"

#include "hal_flash.h"
#include "proto/crc32.h"

uint32_t slots_size(void) { return hal_flash_geometry()->slot_size; }

void slots_read(uint8_t slot, uint32_t offset, uint8_t *buf, size_t len)
{
    hal_flash_read(hal_flash_geometry()->slot_addr[slot & 1u] + offset, buf, len);
}

bool slots_write(uint8_t slot, uint32_t offset, const uint8_t *data, size_t len, uint32_t *erased)
{
    const hal_flash_geometry_t *g = hal_flash_geometry();
    if ((offset | len) & 1u || offset + len > g->slot_size)
        return false;
    uint32_t base = g->slot_addr[slot & 1u];
    uint32_t first = offset / g->page_size;
    uint32_t last = len ? (offset + (uint32_t)len - 1u) / g->page_size : first;
    for (uint32_t p = first; p <= last && len; p++) {
        if (!(*erased & (1u << p))) {
            if (!hal_flash_erase_page(base + p * g->page_size))
                return false;
            *erased |= 1u << p;
        }
    }
    return len == 0 || hal_flash_program(base + offset, data, len);
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
