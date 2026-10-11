/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Package storage in the external W25Q64. Addresses are W25Q byte addresses. Layout:
 *   0x000000  4 × 16 KB  package slots 0…3: one active, the others kept erased in the background,
 *                        so even back-to-back uploads never wait for an erase
 *   0x010000      4 KB   state journal, page A
 *   0x011000      4 KB   state journal, page B
 *   0x012000…            free (several stored packages: a later protocol version)
 * A 16 KB slot holds the largest possible package (4096 words: 8836 bytes). The "page" of the core
 * is the 4 KB erase sector. Programs are verified by reading back. */
#include "f411.h"
#include "hal_flash.h"

static const hal_flash_geometry_t geometry = {
    .page_size = 4096u,
    .slot_count = 4,
    .slot_addr = {0x000000u, 0x004000u, 0x008000u, 0x00C000u},
    .slot_size = 16384u,
    .journal_addr = {0x010000u, 0x011000u},
};

#define AREA_END 0x012000u

const hal_flash_geometry_t *hal_flash_geometry(void) { return &geometry; }

bool hal_flash_erase_page(uint32_t addr)
{
    if (addr + geometry.page_size > AREA_END || (addr % geometry.page_size) || !w25q_erase_sector(addr))
        return false;
    uint8_t buf[64];
    for (uint32_t off = 0; off < geometry.page_size; off += sizeof buf) {
        w25q_read(addr + off, buf, sizeof buf);
        for (unsigned i = 0; i < sizeof buf; i++)
            if (buf[i] != 0xFF)
                return false;
    }
    return true;
}

bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (((addr | len) & 1u) || addr + len > AREA_END)
        return false;
    uint8_t buf[64];
    /* NOR could program over programmed bytes; the HAL contract requires erased targets */
    for (size_t off = 0; off < len; off += sizeof buf) {
        size_t n = len - off < sizeof buf ? len - off : sizeof buf;
        w25q_read(addr + (uint32_t)off, buf, (uint32_t)n);
        for (size_t i = 0; i < n; i++)
            if (buf[i] != 0xFF)
                return false;
    }
    if (!w25q_program(addr, data, (uint32_t)len))
        return false;
    for (size_t off = 0; off < len; off += sizeof buf) {
        size_t n = len - off < sizeof buf ? len - off : sizeof buf;
        w25q_read(addr + (uint32_t)off, buf, (uint32_t)n);
        for (size_t i = 0; i < n; i++)
            if (buf[i] != data[off + i])
                return false;
    }
    return true;
}

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len) { w25q_read(addr, buf, (uint32_t)len); }

static bool bg_running, bg_failed;

bool hal_flash_erase_start(uint32_t addr)
{
    bg_failed = addr + geometry.page_size > AREA_END || (addr % geometry.page_size) || !w25q_erase_start(addr);
    bg_running = !bg_failed;
    return !bg_failed;
}

hal_erase_status_t hal_flash_erase_status(void)
{
    if (bg_running) {
        if (!w25q_erase_done())
            return HAL_ERASE_BUSY;
        bg_running = false;
        return w25q_erase_timed_out() ? HAL_ERASE_FAILED : HAL_ERASE_DONE;   /* the core blank-checks */
    }
    if (bg_failed) {
        bg_failed = false;
        return HAL_ERASE_FAILED;
    }
    return HAL_ERASE_IDLE;
}
