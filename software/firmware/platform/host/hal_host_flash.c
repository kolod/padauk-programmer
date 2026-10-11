/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Flash emulation with F072 geometry, optional file backing and power-fail injection. */
#include "hal_flash.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE (64u * 1024u)
#define POWER_FAIL_EXIT 99

static uint8_t mem[FLASH_SIZE];
static FILE *backing;
static long fail_after = -1;
static bool corrupt_writes;

static const hal_flash_geometry_t geometry = {
    .page_size = 2048u,
    .slot_count = 2,
    .slot_addr = {0x0800B000u, 0x0800D000u},
    .slot_size = 8192u,
    .journal_addr = {0x0800F000u, 0x0800F800u},
};

const hal_flash_geometry_t *hal_flash_geometry(void) { return &geometry; }

bool host_flash_open(const char *path)
{
    memset(mem, 0xFF, sizeof mem);
    if (!path)
        return true;
    backing = fopen(path, "r+b");
    if (backing) {
        size_t n = fread(mem, 1, sizeof mem, backing);
        if (n != sizeof mem)
            memset(mem + n, 0xFF, sizeof mem - n);
    } else {
        backing = fopen(path, "w+b");
        if (!backing)
            return false;
        fwrite(mem, 1, sizeof mem, backing);
    }
    fflush(backing);
    return true;
}

void host_flash_power_fail_after(long n) { fail_after = n; }
void host_flash_corrupt_during_write(bool on) { corrupt_writes = on; }

static void persist(uint32_t off, size_t len)
{
    if (!backing)
        return;
    fseek(backing, (long)off, SEEK_SET);
    fwrite(mem + off, 1, len, backing);
    fflush(backing);
}

/* Returns true if this operation is the one interrupted by the simulated power loss. */
static bool power_fail_now(void)
{
    if (fail_after < 0)
        return false;
    if (fail_after == 0)
        return true;
    fail_after--;
    return false;
}

static void die_power_fail(void)
{
    if (backing)
        fclose(backing);
    fprintf(stderr, "simulated power loss\n");
    exit(POWER_FAIL_EXIT);
}

bool hal_flash_erase_page(uint32_t addr)
{
    if (addr < FLASH_BASE || addr + geometry.page_size > FLASH_BASE + FLASH_SIZE || (addr - FLASH_BASE) % geometry.page_size)
        return false;
    uint32_t off = addr - FLASH_BASE;
    bool torn = power_fail_now();
    memset(mem + off, 0xFF, torn ? geometry.page_size / 2 : geometry.page_size);
    persist(off, geometry.page_size);
    if (torn)
        die_power_fail();
    return true;
}

bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if ((addr | len) & 1u || addr < FLASH_BASE || addr + len > FLASH_BASE + FLASH_SIZE)
        return false;
    uint32_t off = addr - FLASH_BASE;
    bool torn = power_fail_now();
    size_t n = torn ? (len / 4u) * 2u : len;
    bool ok = true;
    for (size_t i = 0; i < n; i += 2) {
        if (mem[off + i] != 0xFF || mem[off + i + 1] != 0xFF) {
            ok = false;
            break;
        }
        mem[off + i] = data[i];
        mem[off + i + 1] = data[i + 1];
    }
    persist(off, len);
    if (torn)
        die_power_fail();
    return ok;
}

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
    memcpy(buf, mem + (addr - FLASH_BASE), len);
    if (corrupt_writes && len)
        buf[0] ^= 0x01;   /* simulated read disturbance (FR-049 tests) */
}

/* No background erase on this flash: the erase completes inside erase_start(). */
static hal_erase_status_t erase_result = HAL_ERASE_IDLE;

bool hal_flash_erase_start(uint32_t addr)
{
    bool ok = hal_flash_erase_page(addr);
    erase_result = ok ? HAL_ERASE_DONE : HAL_ERASE_FAILED;
    return ok;
}

hal_erase_status_t hal_flash_erase_status(void)
{
    hal_erase_status_t st = erase_result;
    erase_result = HAL_ERASE_IDLE;
    return st;
}
