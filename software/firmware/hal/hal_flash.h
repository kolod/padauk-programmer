/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Non-volatile storage for package slots and the state journal (internal or external flash).
 * The layout is set by the platform. */
#ifndef PDK_HAL_FLASH_H
#define PDK_HAL_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAL_FLASH_MAX_SLOTS 4u

typedef struct {
    uint32_t page_size;                        /* erase granularity, bytes */
    uint8_t write_unit;                        /* program granularity, bytes: 1, 2, 4 or 8 */
    uint8_t slot_count;                        /* 2 … HAL_FLASH_MAX_SLOTS package slots */
    uint32_t slot_addr[HAL_FLASH_MAX_SLOTS];
    uint32_t slot_size;                        /* bytes per slot (multiple of page_size, ≤ 32 pages) */
    uint32_t journal_addr[2];                  /* two journal pages */
} hal_flash_geometry_t;

const hal_flash_geometry_t *hal_flash_geometry(void);

/* Erases the page that starts at `addr` and returns when done. Waits for a background erase first. */
bool hal_flash_erase_page(uint32_t addr);

/* Starts erasing the page at `addr` in the background (at most one at a time). Platforms whose flash
 * cannot erase in the background finish the erase before returning. The result is then reported by
 * hal_flash_erase_status(). While a background erase runs, other pages may be read and programmed. */
bool hal_flash_erase_start(uint32_t addr);

typedef enum { HAL_ERASE_IDLE = 0, HAL_ERASE_BUSY, HAL_ERASE_DONE, HAL_ERASE_FAILED } hal_erase_status_t;

/* Status of the last hal_flash_erase_start(); DONE/FAILED are reported once, then IDLE. */
hal_erase_status_t hal_flash_erase_status(void);

/* Programs `len` bytes; addr and len are multiples of write_unit. Target bytes must be erased (0xFF)
 * and every write unit is programmed once (ECC flash). */
bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len);

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len);

#endif
