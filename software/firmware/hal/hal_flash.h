/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal flash used for package slots and the state journal. The layout is set by the platform. */
#ifndef PDK_HAL_FLASH_H
#define PDK_HAL_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t page_size;          /* erase granularity, bytes */
    uint32_t slot_addr[2];       /* package slots */
    uint32_t slot_size;          /* bytes per slot (multiple of page_size) */
    uint32_t journal_addr[2];    /* two journal pages */
} hal_flash_geometry_t;

const hal_flash_geometry_t *hal_flash_geometry(void);

/* Erases the page that starts at `addr`. */
bool hal_flash_erase_page(uint32_t addr);

/* Programs `len` bytes (addr and len even). Target bytes must be erased (0xFF). */
bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len);

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len);

#endif
