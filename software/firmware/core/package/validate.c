/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "validate.h"

#include <string.h>

#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/slots.h"

void pkg_slot_reader_init(pkg_slot_reader_t *r, uint8_t slot, uint16_t image_words)
{
    r->slot = slot;
    r->image_words = image_words;
    r->mask_offset = pkg_mask_offset(image_words);
    uint32_t mask_bytes = 2u * ((image_words + 15u) / 16u);
    if (mask_bytes > PKG_READER_MASK_MAX)
        mask_bytes = PKG_READER_MASK_MAX;
    memset(r->mask, 0, sizeof r->mask);
    slots_read(slot, r->mask_offset, r->mask, mask_bytes);
    r->win_first = 0xFFFF;
}

uint16_t pkg_slot_word(void *reader, uint16_t addr, bool *defined)
{
    pkg_slot_reader_t *r = reader;
    *defined = false;
    if (addr >= r->image_words || addr / 8u >= PKG_READER_MASK_MAX || !(r->mask[addr / 8u] & (1u << (addr % 8u))))
        return 0xFFFF;
    if (r->win_first == 0xFFFF || addr < r->win_first || addr >= r->win_first + PKG_READER_WINDOW) {
        uint16_t first = (uint16_t)(addr - addr % PKG_READER_WINDOW);
        uint32_t n = (uint32_t)r->image_words - first;
        if (n > PKG_READER_WINDOW)
            n = PKG_READER_WINDOW;
        uint8_t buf[2u * PKG_READER_WINDOW];
        slots_read(r->slot, PKG_HEADER_SIZE + 2u * first, buf, 2u * n);
        for (uint32_t i = 0; i < n; i++)
            r->win[i] = le16(buf + 2u * i);
        r->win_first = first;
    }
    *defined = true;
    return r->win[addr - r->win_first];
}

uint16_t pkg_validate(const pkg_header_t *h, const chip_info_t *c, word_get_fn get, void *ctx,
                      pkg_plan_t *plan, uint16_t *bad_addr)
{
    memset(plan, 0, sizeof *plan);
    *bad_addr = 0xFFFF;
    uint16_t mask = chip_word_mask(c);
    uint16_t fuse_addr = chip_fuse_addr(c);
    /* SECURE_FILL: unused words below fill_end become NOP (up to the reserved area or code_words - 8). */
    bool fill = (h->flags & PKG_F_SECURE_FILL) != 0;
    uint16_t fill_end = (uint16_t)(c->code_words - 8u);
    if (c->excl_end > c->excl_start && c->excl_start < fill_end)
        fill_end = c->excl_start;
    uint16_t written = fill ? fill_end : 0;

    for (uint16_t a = 0; a < h->image_words; a++) {
        bool def = false;
        uint16_t w = get(ctx, a, &def);
        if (!def)
            continue;
        if (chip_addr_reserved(c, a)) {
            *bad_addr = a;
            return PDK_PKG_RESERVED_AREA;
        }
        if (a == fuse_addr) {
            if ((h->flags & PKG_F_FUSE_PRESENT) && ((h->fuse & mask) != (w & mask))) {
                *bad_addr = a;
                return PDK_PKG_FUSE_CONFLICT;
            }
            plan->fuse_defined = true;
            plan->fuse = w & mask;
            continue;
        }
        if (!fill || a >= fill_end)
            written++;
    }
    if (h->flags & PKG_F_FUSE_PRESENT) {
        plan->fuse_defined = true;
        plan->fuse = h->fuse & mask;
    }
    if (plan->fuse_defined && pkg_fuse_is_secure(plan->fuse) && !(h->flags & PKG_F_ALLOW_SECURITY_FUSE)) {
        *bad_addr = fuse_addr;
        return PDK_PKG_SECURITY_NOT_ALLOWED;
    }

    uint16_t st = calib_find_sites(c->code_bits, get, ctx, h->image_words, plan->calib, &plan->calib_count, bad_addr);
    if (st != PDK_OK)
        return st;
    plan->serial_count = serial_find_sites(c->code_bits, get, ctx, h->image_words, plan->serial_addr, SERIAL_MAX_SITES);

    plan->effective_words = written;
    return PDK_OK;
}
