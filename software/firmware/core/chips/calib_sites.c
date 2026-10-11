/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Placeholder layout ported from easy-pdk-programmer-software/fpdkiccalib.c and
 * Examples/src/easypdk/pdkcommon.h, Copyright (C) 2019-2020 freepdk https://free-pdk.github.io
 * (GPL-3.0-or-later). Only the search part is here; loop/finalisation opcodes come with the
 * effective image.
 */
#include "calib_sites.h"

#include "proto/proto_defs.h"

uint16_t calib_and_opcode(uint8_t code_bits)
{
    switch (code_bits) {
    case 13: return 0x1400;
    case 14: return 0x2C00;
    case 15: return 0x5400;
    case 16: return 0x1C00;
    default: return 0;
    }
}

typedef struct {
    word_get_fn get;
    void *ctx;
    uint16_t count;
    uint16_t and_op;
} scan_t;

/* Returns the immediate if the word at `addr` is `AND A,#imm`, else -1. */
static int and_imm(const scan_t *s, uint32_t addr)
{
    if (addr >= s->count)
        return -1;
    bool def = false;
    uint16_t w = s->get(s->ctx, (uint16_t)addr, &def);
    if (!def || (w & 0xFF00u) != s->and_op)
        return -1;
    return w & 0xFF;
}

static int match_rc(const scan_t *s, uint16_t a, calib_site_t *site)
{
    int v[CALIB_RC_LEN];
    for (unsigned i = 0; i < CALIB_RC_LEN; i++)
        if ((v[i] = and_imm(s, (uint32_t)a + i)) < 0)
            return 0;
    if (v[0] != 'R' || v[1] != 'C' || (v[2] != CALIB_KIND_IHRC && v[2] != CALIB_KIND_ILRC))
        return 0;
    site->addr = a;
    site->kind = (uint8_t)v[2];
    site->len = CALIB_RC_LEN;
    site->target_hz = (uint32_t)v[3] | ((uint32_t)v[4] << 8) | ((uint32_t)v[5] << 16) | ((uint32_t)v[6] << 24);
    site->mV = (uint16_t)(v[7] | (v[8] << 8));
    site->reg0 = (uint8_t)v[9];
    site->reg1 = site->reg2 = 0;
    return 1;
}

static int match_bg(const scan_t *s, uint16_t a, calib_site_t *site)
{
    int v[CALIB_BG_LEN];
    for (unsigned i = 0; i < CALIB_BG_LEN; i++)
        if ((v[i] = and_imm(s, (uint32_t)a + i)) < 0)
            return 0;
    if (v[0] != 'B' || v[1] != 'G' || v[2] != CALIB_KIND_BG)
        return 0;
    for (unsigned i = 8; i < CALIB_BG_LEN; i++)
        if (v[i] != 0)
            return 0;
    site->addr = a;
    site->kind = CALIB_KIND_BG;
    site->len = CALIB_BG_LEN;
    site->target_hz = 0;
    site->mV = (uint16_t)(v[3] | (v[4] << 8));
    site->reg0 = (uint8_t)v[5];
    site->reg1 = (uint8_t)v[6];
    site->reg2 = (uint8_t)v[7];
    return 1;
}

uint16_t calib_find_sites(uint8_t code_bits, word_get_fn get, void *ctx, uint16_t count,
                          calib_site_t *sites, uint8_t *n_sites, uint16_t *bad_addr)
{
    scan_t s = {get, ctx, count, calib_and_opcode(code_bits)};
    *n_sites = 0;
    if (!s.and_op)
        return PDK_OK;

    for (uint32_t a = 0; a < count;) {
        calib_site_t site;
        if (and_imm(&s, a) < 0) {
            a++;
            continue;
        }
        if (code_bits <= 15 && (match_rc(&s, (uint16_t)a, &site) || match_bg(&s, (uint16_t)a, &site))) {
            if (*n_sites >= CALIB_MAX_SITES) {
                *bad_addr = (uint16_t)a;
                return PDK_BAD_PAYLOAD;
            }
            sites[(*n_sites)++] = site;
            a += site.len;
            continue;
        }
        /* Run of AND A,# words that does not start a known placeholder. */
        uint32_t run = 0;
        while (and_imm(&s, a + run) >= 0) {
            calib_site_t probe;
            if (run && code_bits <= 15 &&
                (match_rc(&s, (uint16_t)(a + run), &probe) || match_bg(&s, (uint16_t)(a + run), &probe)))
                break;
            run++;
        }
        if (run >= CALIB_UNKNOWN_RUN) {
            *bad_addr = (uint16_t)a;
            return PDK_PKG_CALIB_UNKNOWN;
        }
        a += run;
    }
    return PDK_OK;
}
