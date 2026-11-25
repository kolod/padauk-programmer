/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_crc.h"

#include <string.h>

#include "chips/calib_footprint.h"
#include "chips/serial_sites.h"
#include "proto/crc32.h"

void image_crc_init(image_crc_t *c, const chip_info_t *chip)
{
    memset(c, 0, sizeof *c);
    c->chip = chip;
    c->mask = chip_word_mask(chip);
    c->ret_op = serial_ret_opcode(chip->code_bits);
    c->raw = c->norm = crc32_init();
}

static bool counted(const image_crc_t *c, uint16_t addr)
{
    return !(c->chip->excl_first && addr == 0) && !chip_addr_reserved(c->chip, addr);
}

static uint32_t add_word(uint32_t crc, uint16_t w)
{
    uint8_t b[2] = {(uint8_t)w, (uint8_t)(w >> 8)};
    return crc32_update(crc, b, 2);
}

static bool is_ret(const image_crc_t *c, uint16_t w)
{
    return c->ret_op && (w & 0xFF00u) == c->ret_op;
}

/* Passes `n` words from the head of the window to the norm sum. */
static void emit(image_crc_t *c, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (counted(c, (uint16_t)(c->base + i)))
            c->norm = add_word(c->norm, c->win[i]);
    c->prev_ret = is_ret(c, c->win[n - 1]);
    memmove(c->win, c->win + n, 2u * (c->n - n));
    c->n = (uint8_t)(c->n - n);
    c->base = (uint16_t)(c->base + n);
}

/* Decides on the word at the head of the window; `end` = no more words will come. */
static void step(image_crc_t *c, bool end)
{
    calib_match_t m;
    if (calib_match(c->chip, c->win, c->n, c->base, &m)) {
        if (m.form == CALIB_FORM_FOOTPRINT)
            c->win[m.kind == CALIB_KIND_BG ? 7 : 2] &= 0xFF00u;
        emit(c, m.len);   /* a loop is passed as a unit too, as the scan of a dump does */
        return;
    }
    if (is_ret(c, c->win[0]) && !c->prev_ret) {
        unsigned run = 1;
        while (run < c->n && run <= SERIAL_SITE_LEN && is_ret(c, c->win[run]))
            run++;
        if (run == SERIAL_SITE_LEN && (c->n > SERIAL_SITE_LEN || end)) {
            for (unsigned i = 0; i < SERIAL_SITE_LEN; i++)
                c->win[i] &= 0xFF00u;
            emit(c, SERIAL_SITE_LEN);
            return;
        }
    }
    emit(c, 1);
}

void image_crc_feed(image_crc_t *c, uint16_t word)
{
    uint16_t w = (uint16_t)(word & c->mask);
    if (counted(c, c->next_addr))
        c->raw = add_word(c->raw, w);
    c->next_addr++;
    c->win[c->n++] = w;
    if (c->n == IMAGE_CRC_WINDOW)
        step(c, false);
}

void image_crc_finish(image_crc_t *c, uint32_t *raw, uint32_t *norm)
{
    while (c->n)
        step(c, true);
    *raw = crc32_final(c->raw);
    *norm = crc32_final(c->norm);
}

void image_crc_of_eff(const eff_image_t *e, uint32_t *raw, uint32_t *norm)
{
    image_crc_t c;
    image_crc_init(&c, e->chip);
    for (uint16_t a = 0; a < e->chip->code_words; a++)
        image_crc_feed(&c, eff_word(e, a));
    image_crc_finish(&c, raw, norm);
}
