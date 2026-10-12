/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Package storage in flash bank 2 of the STM32G474 (RM0440 §3). Layout:
 *   0x08040000  4 × 16 KB  package slots 0…3 (one active, the others kept erased in the background)
 *   0x08050000             state journal, page A
 *   0x08051000             state journal, page B
 * Dual-bank mode (DBANK = 1, the factory default): 2 KB pages, and the CPU keeps running from bank 1
 * while bank 2 is erased or programmed, so the background erase is truly asynchronous. Single-bank
 * mode (DBANK = 0) also works with 4 KB pages, but then erases stall the CPU.
 * Programming is in 64-bit double words, each written once (ECC); the core buffers partial words. */
#include <string.h>

#include "g474.h"
#include "hal_flash.h"

#define AREA_START 0x08040000u
#define AREA_END 0x08052000u
#define SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
                   FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR | FLASH_SR_OPTVERR)

static hal_flash_geometry_t geometry = {
    .page_size = 2048u,
    .write_unit = 8,
    .slot_count = 4,
    .slot_addr = {0x08040000u, 0x08044000u, 0x08048000u, 0x0804C000u},
    .slot_size = 16384u,
    .journal_addr = {0x08050000u, 0x08051000u},
};

static bool dual_bank;
static bool erasing;            /* a background erase is running (or finished, not yet collected) */
static bool start_failed;
static bool result_ready;       /* collected early (e.g. by a program), not yet reported */
static bool result_ok;

const hal_flash_geometry_t *hal_flash_geometry(void) { return &geometry; }

void g474_flash_init(void)
{
    dual_bank = (FLASH->OPTR & FLASH_OPTR_DBANK) != 0;
    geometry.page_size = dual_bank ? 2048u : 4096u;
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
}

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static void lock(void) { FLASH->CR |= FLASH_CR_LOCK; }

static void wait_idle(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
}

/* Flash contents changed: drop what the data cache holds (instructions live in bank 1). */
static void flush_dcache(void)
{
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

static bool in_area(uint32_t addr, uint32_t len) { return addr >= AREA_START && addr + len <= AREA_END && addr + len >= addr; }

/* Waits for a running erase and keeps its result for hal_flash_erase_status(). */
static void collect_erase(void)
{
    if (!erasing)
        return;
    wait_idle();
    result_ok = !(FLASH->SR & SR_ERRORS);
    result_ready = true;
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER);
    lock();
    flush_dcache();
    erasing = false;
}

bool hal_flash_erase_start(uint32_t addr)
{
    start_failed = !in_area(addr, geometry.page_size) || (addr % geometry.page_size);
    if (start_failed)
        return false;
    collect_erase();   /* one erase at a time */
    result_ready = false;
    wait_idle();       /* a program may still be finishing */
    uint32_t page, bank = 0;
    if (dual_bank) {
        bank = addr >= 0x08040000u;
        page = (addr - (bank ? 0x08040000u : 0x08000000u)) / 2048u;
    } else {
        page = (addr - 0x08000000u) / 4096u;
    }
    unlock();
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_BKER | FLASH_CR_PG)) | FLASH_CR_PER |
                (page << FLASH_CR_PNB_Pos) | (bank ? FLASH_CR_BKER : 0u);
    FLASH->CR |= FLASH_CR_STRT;
    erasing = true;
    return true;
}

hal_erase_status_t hal_flash_erase_status(void)
{
    if (start_failed) {
        start_failed = false;
        return HAL_ERASE_FAILED;
    }
    if (erasing && (FLASH->SR & FLASH_SR_BSY))
        return HAL_ERASE_BUSY;
    collect_erase();
    if (!result_ready)
        return HAL_ERASE_IDLE;
    result_ready = false;
    return result_ok ? HAL_ERASE_DONE : HAL_ERASE_FAILED;
}

bool hal_flash_erase_page(uint32_t addr)
{
    if (!hal_flash_erase_start(addr))   /* waits for a background erase first */
        return false;
    collect_erase();
    result_ready = false;
    if (!result_ok)
        return false;
    const uint32_t *p = (const uint32_t *)addr;
    for (uint32_t i = 0; i < geometry.page_size / 4u; i++)
        if (p[i] != 0xFFFFFFFFu)
            return false;
    return true;
}

bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (((addr | len) & 7u) || !in_area(addr, (uint32_t)len))
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < len; i += 8) {
        volatile uint32_t *dst = (volatile uint32_t *)(addr + i);
        if (dst[0] != 0xFFFFFFFFu || dst[1] != 0xFFFFFFFFu) {
            ok = false;   /* a double word can be programmed only once after an erase */
            break;
        }
        uint32_t lo, hi;
        memcpy(&lo, data + i, 4);
        memcpy(&hi, data + i + 4, 4);
        collect_erase();   /* a background erase must finish first; its result is kept */
        wait_idle();
        unlock();
        FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
        FLASH->CR = (FLASH->CR & ~(FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER)) | FLASH_CR_PG;
        dst[0] = lo;
        __ISB();
        dst[1] = hi;
        wait_idle();
        ok = !(FLASH->SR & SR_ERRORS);
        FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
        FLASH->CR &= ~FLASH_CR_PG;
        flush_dcache();
        ok = ok && dst[0] == lo && dst[1] == hi;
    }
    if (!erasing)
        lock();
    return ok;
}

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len) { memcpy(buf, (const void *)addr, len); }
