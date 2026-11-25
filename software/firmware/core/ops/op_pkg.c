/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "op_pkg.h"

#include "proto/proto_defs.h"
#include "storage/package_store.h"
#include "storage/slots.h"

op_pkg_t op_pkg;

uint16_t op_pkg_load(void)
{
    op_pkg_t *p = &op_pkg;
    if (package_store_state() == PDK_PKG_EMPTY)
        return PDK_STORE_EMPTY;
    uint32_t crc;
    if (package_store_recheck(&crc) != PDK_PKG_VALID)
        return PDK_STORE_CORRUPTED;
    uint8_t raw[PKG_HEADER_SIZE];
    package_store_read(0, raw, sizeof raw);
    if (pkg_header_parse(raw, false, slots_size(), &p->hdr, &p->chip) != PDK_OK)
        return PDK_STORE_CORRUPTED;
    pkg_slot_reader_init(&p->reader, package_store_active_slot(), p->hdr.image_words);
    uint16_t bad;
    if (pkg_validate(&p->hdr, p->chip, pkg_slot_word, &p->reader, &p->plan, &bad) != PDK_OK)
        return PDK_STORE_CORRUPTED;
    eff_init(&p->eff, p->chip, &p->hdr, &p->plan, pkg_slot_word, &p->reader);
    p->mask = chip_word_mask(p->chip);
    return PDK_OK;
}

bool op_pkg_counted(const chip_info_t *chip, uint16_t addr)
{
    return !(chip->excl_first && addr == 0) && !chip_addr_reserved(chip, addr);
}
