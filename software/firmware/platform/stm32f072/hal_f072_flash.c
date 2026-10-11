/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal flash: 2 KB pages, half-word programming (RM0091 §3). Layout in stm32f072c8.ld. */
#include <string.h>

#include "f072.h"
#include "hal_flash.h"

static const hal_flash_geometry_t geometry = {
    .page_size = 2048u,
    .slot_addr = {0x0800B000u, 0x0800D000u},
    .slot_size = 8192u,
    .journal_addr = {0x0800F000u, 0x0800F800u},
};

const hal_flash_geometry_t *hal_flash_geometry(void) { return &geometry; }

static bool in_data_area(uint32_t addr, uint32_t len)
{
    return addr >= geometry.slot_addr[0] && addr + len <= 0x08010000u && addr + len >= addr;
}

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static bool wait_done(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
    bool ok = !(FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPERR));
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPERR;
    return ok;
}

bool hal_flash_erase_page(uint32_t addr)
{
    if (!in_data_area(addr, geometry.page_size) || (addr & (geometry.page_size - 1u)))
        return false;
    unlock();
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = addr;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = wait_done();
    FLASH->CR &= ~FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_LOCK;
    const uint32_t *p = (const uint32_t *)addr;
    for (uint32_t i = 0; ok && i < geometry.page_size / 4u; i++)
        ok = p[i] == 0xFFFFFFFFu;
    return ok;
}

bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (((addr | len) & 1u) || !in_data_area(addr, (uint32_t)len))
        return false;
    unlock();
    FLASH->CR |= FLASH_CR_PG;
    bool ok = true;
    for (size_t i = 0; ok && i < len; i += 2) {
        uint16_t v = (uint16_t)(data[i] | (data[i + 1] << 8));
        volatile uint16_t *dst = (volatile uint16_t *)(addr + i);
        if (*dst != 0xFFFFu) {
            ok = false;
            break;
        }
        *dst = v;
        ok = wait_done() && *dst == v;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_LOCK;
    return ok;
}

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len) { memcpy(buf, (const void *)addr, len); }
