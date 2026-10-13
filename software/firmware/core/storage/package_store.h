/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Active package: which slot, size, CRC and integrity state (data-model PackageState). */
#ifndef PDK_PACKAGE_STORE_H
#define PDK_PACKAGE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Loads the journal and verifies the active package (state EMPTY / VALID / CORRUPTED). */
void package_store_init(void);

uint8_t package_store_state(void);
uint32_t package_store_size(void);
uint32_t package_store_crc(void);
uint8_t package_store_active_slot(void);
/* Slot for the next upload: a clean one other than the active slot when possible. */
uint8_t package_store_upload_slot(void);
uint16_t package_store_flags(void);

/* Re-reads the active slot; returns the new state and the computed CRC. */
uint8_t package_store_recheck(uint32_t *computed_crc);

/* Makes `slot` the active package (one journal record). */
bool package_store_commit(uint8_t slot, uint32_t size, uint32_t crc);
bool package_store_delete(void);

/* Stores the supply calibration (power.h POWER_CAL_*) in the journal; valid = false clears it. */
bool package_store_save_supply_cal(const int8_t cal[4], bool valid);

void package_store_read(uint32_t offset, uint8_t *buf, size_t len);

#endif
