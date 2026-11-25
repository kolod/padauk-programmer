/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "effective_image.h"

#include <string.h>

#include "chips/calib_footprint.h"

void eff_init(eff_image_t *e, const chip_info_t *chip, const pkg_header_t *h, const pkg_plan_t *plan,
              word_get_fn get, void *ctx)
{
    memset(e, 0, sizeof *e);
    e->chip = chip;
    e->plan = plan;
    e->get = get;
    e->ctx = ctx;
    e->secure_fill = (h->flags & PKG_F_SECURE_FILL) != 0;
    e->fill_end = pkg_fill_end(chip);
    memset(e->value, CALIB_LOOP_IMM, sizeof e->value);
}

void eff_set_loop(eff_image_t *e, uint8_t site, uint8_t imm)
{
    e->final_mask &= (uint16_t)~(1u << site);
    e->value[site] = imm;
}

void eff_set_final(eff_image_t *e, uint8_t site, uint8_t k)
{
    e->final_mask |= (uint16_t)(1u << site);
    e->value[site] = k;
}

void eff_set_serial(eff_image_t *e, const uint8_t serial[EFF_SERIAL_BYTES])
{
    memcpy(e->serial, serial, EFF_SERIAL_BYTES);
}

eff_class_t eff_classify(const eff_image_t *e, uint16_t addr)
{
    const pkg_plan_t *p = e->plan;
    if (addr == chip_fuse_addr(e->chip))
        return (eff_class_t){EFF_FUSE, 0, 0};
    if (chip_addr_reserved(e->chip, addr))
        return (eff_class_t){EFF_RESERVED, 0, 0};
    for (uint8_t i = 0; i < p->calib_count; i++)
        if (addr >= p->calib[i].addr && addr < p->calib[i].addr + p->calib[i].len)
            return (eff_class_t){EFF_CALIB, i, (uint8_t)(addr - p->calib[i].addr)};
    for (uint8_t i = 0; i < p->serial_count; i++)
        if (addr >= p->serial_addr[i] && addr < p->serial_addr[i] + SERIAL_SITE_LEN)
            return (eff_class_t){EFF_SERIAL, i, (uint8_t)(addr - p->serial_addr[i])};
    bool def = false;
    e->get(e->ctx, addr, &def);
    if (def || (e->secure_fill && addr < e->fill_end))
        return (eff_class_t){EFF_PLAIN, 0, 0};
    return (eff_class_t){EFF_UNWRITTEN, 0, 0};
}

uint16_t eff_word(const eff_image_t *e, uint16_t addr)
{
    uint16_t mask = chip_word_mask(e->chip);
    eff_class_t c = eff_classify(e, addr);
    switch (c.kind) {
    case EFF_PLAIN: {
        bool def = false;
        uint16_t w = e->get(e->ctx, addr, &def);
        return def ? (uint16_t)(w & mask) : 0x0000;   /* SECURE_FILL: NOP */
    }
    case EFF_FUSE:
        return e->plan->fuse_defined ? e->plan->fuse : mask;
    case EFF_CALIB: {
        uint16_t w[CALIB_BG_LEN];
        const calib_site_t *s = &e->plan->calib[c.index];
        if (e->final_mask & (1u << c.index))
            calib_final_words(e->chip->code_bits, s, e->value[c.index], w);
        else
            calib_loop_words(e->chip->code_bits, s, e->value[c.index], w);
        return (uint16_t)(w[c.offset] & mask);
    }
    case EFF_SERIAL:
        return (uint16_t)((serial_ret_opcode(e->chip->code_bits) | e->serial[c.offset]) & mask);
    default:
        return mask;
    }
}
