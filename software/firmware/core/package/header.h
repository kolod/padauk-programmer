/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Package header (contracts/package-format.md). */
#ifndef PDK_PKG_HEADER_H
#define PDK_PKG_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"

#define PKG_HEADER_SIZE 128u
#define PKG_FORMAT_VERSION 1u

enum {
    PKG_F_FUSE_PRESENT = 1u << 0,
    PKG_F_ALLOW_SECURITY_FUSE = 1u << 1,
    PKG_F_SKIP_ERASE = 1u << 2,
    PKG_F_SKIP_BLANK_CHECK = 1u << 3,
    PKG_F_SKIP_VERIFY = 1u << 4,
    PKG_F_SECURE_FILL = 1u << 6,
    PKG_F_CREATED_BY_COPY = 1u << 8,
};
#define PKG_F_HOST_ALLOWED (PKG_F_FUSE_PRESENT | PKG_F_ALLOW_SECURITY_FUSE | PKG_F_SKIP_ERASE | \
                            PKG_F_SKIP_BLANK_CHECK | PKG_F_SKIP_VERIFY | PKG_F_SECURE_FILL)
#define PKG_F_INTERNAL_ALLOWED (PKG_F_HOST_ALLOWED | PKG_F_CREATED_BY_COPY)

typedef struct {
    uint32_t package_size;
    uint16_t chip_id12;
    uint16_t image_words;
    uint32_t flags;
    uint16_t fuse;
    uint16_t run_vdd_mV;
    uint32_t source_crc32;
    uint32_t created_unix;
    char chip_name[17];
    char name[33];
    char label[29];
} pkg_header_t;

/* package_size for N image words: header + words + mask (whole u16) + trailer CRC. */
uint32_t pkg_expected_size(uint16_t image_words);
uint32_t pkg_mask_offset(uint16_t image_words);

/* Parses and checks the header (PKG_BEGIN rules). `from_host` forbids internal-only flags.
 * `max_package_size` is the slot capacity. On success sets *chip. */
uint16_t pkg_header_parse(const uint8_t *raw, bool from_host, uint32_t max_package_size,
                          pkg_header_t *h, const chip_info_t **chip);

/* True if the fuse value enables read protection (bit 0 = 0). */
static inline bool pkg_fuse_is_secure(uint16_t fuse) { return (fuse & 1u) == 0; }

#endif
