/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Measurement loop and post-calibration opcodes ported from easy-pdk-programmer-software/fpdkiccalib.c,
 * Copyright (C) 2019-2020 freepdk https://free-pdk.github.io (GPL-3.0-or-later). The footprint
 * recognition and placeholder restoration are this project's (research R9).
 */
#include "calib_footprint.h"

#include <string.h>

/* Per loop word: base opcode and what happens to it. */
enum {
    LW_FIXUP = 0x01,   /* GOTO: add the site address */
    LW_REG0 = 0x02,    /* OR register 0 (IHRCR/ILRCR, BGTR) */
    LW_REG1 = 0x04,    /* OR register 1 (GPCC) */
    LW_REG2 = 0x08,    /* OR register 2 (GPCS) */
    LW_NOP = 0x10,     /* NOP after calibration */
    LW_RESULT = 0x20,  /* MOV A,#imm: the immediate becomes the result after calibration */
};

typedef struct {
    uint16_t op;
    uint8_t fl;
} loop_word_t;

/* RC: SET1 PAC.3, SET1 PADIER.4, MOV A,#FF, ADD A,1, | SET1 PA.3, MOV IO(reg),A, T0SN PA.4, ADD A,1,
 * SET0 PA.3, GOTO +4 |  — 7 cycles per round, PA4 high adds one to the trim value. */
static const loop_word_t rc_loop[3][CALIB_RC_LEN] = {
    {{0x0F71, LW_NOP}, {0x0F8D, LW_NOP}, {0x17FF, LW_RESULT}, {0x1001, LW_NOP}, {0x0F70, LW_NOP},
     {0x0080, LW_REG0}, {0x0C90, LW_NOP}, {0x1001, LW_NOP}, {0x0E70, LW_NOP}, {0x1804, LW_FIXUP | LW_NOP}},
    {{0x1ED1, LW_NOP}, {0x1F0D, LW_NOP}, {0x2FFF, LW_RESULT}, {0x2801, LW_NOP}, {0x1ED0, LW_NOP},
     {0x0180, LW_REG0}, {0x1910, LW_NOP}, {0x2801, LW_NOP}, {0x1CD0, LW_NOP}, {0x3004, LW_FIXUP | LW_NOP}},
    {{0x3D91, LW_NOP}, {0x3E0D, LW_NOP}, {0x57FF, LW_RESULT}, {0x5001, LW_NOP}, {0x3D90, LW_NOP},
     {0x0100, LW_REG0}, {0x3210, LW_NOP}, {0x5001, LW_NOP}, {0x3990, LW_NOP}, {0x6004, LW_FIXUP | LW_NOP}},
};

/* BG: comparator setup (GPCC = 0x84, GPCS = 0xA3, PA0 out), then the same loop on BGTR. */
static const loop_word_t bg_loop[3][CALIB_BG_LEN] = {
    {{0x1709, LW_NOP}, {0x0091, LW_NOP}, {0x0F8D, LW_NOP}, {0x1784, LW_NOP}, {0x0080, LW_REG1 | LW_NOP},
     {0x17A3, LW_NOP}, {0x0080, LW_REG2 | LW_NOP}, {0x17FF, LW_RESULT}, {0x0F70, LW_NOP}, {0x0080, LW_REG0},
     {0x0C90, LW_NOP}, {0x1001, LW_NOP}, {0x0E70, LW_NOP}, {0x1808, LW_FIXUP | LW_NOP}},
    {{0x2F09, LW_NOP}, {0x0191, LW_NOP}, {0x1F0D, LW_NOP}, {0x2F84, LW_NOP}, {0x0180, LW_REG1 | LW_NOP},
     {0x2FA3, LW_NOP}, {0x0180, LW_REG2 | LW_NOP}, {0x2FFF, LW_RESULT}, {0x1ED0, LW_NOP}, {0x0180, LW_REG0},
     {0x1910, LW_NOP}, {0x2801, LW_NOP}, {0x1CD0, LW_NOP}, {0x3008, LW_FIXUP | LW_NOP}},
    {{0x5709, LW_NOP}, {0x0111, LW_NOP}, {0x3E0D, LW_NOP}, {0x5784, LW_NOP}, {0x0100, LW_REG1 | LW_NOP},
     {0x57A3, LW_NOP}, {0x0100, LW_REG2 | LW_NOP}, {0x57FF, LW_RESULT}, {0x3D90, LW_NOP}, {0x0100, LW_REG0},
     {0x3210, LW_NOP}, {0x5001, LW_NOP}, {0x3990, LW_NOP}, {0x6008, LW_FIXUP | LW_NOP}},
};

/* Positions of MOV A,#k and the trim register write in the finished form. */
#define RC_K_POS 2u
#define RC_REG_POS 5u
#define BG_K_POS 7u
#define BG_REG_POS 9u

uint16_t calib_mov_a_imm(uint8_t code_bits)
{
    switch (code_bits) {
    case 13: return 0x1700;
    case 14: return 0x2F00;
    case 15: return 0x5700;
    default: return 0;
    }
}

uint16_t calib_mov_io_a(uint8_t code_bits, uint8_t reg)
{
    switch (code_bits) {
    case 13: return (uint16_t)(0x0080u | (reg & 0x1Fu));
    case 14: return (uint16_t)(0x0180u | (reg & 0x3Fu));
    case 15: return (uint16_t)(0x0100u | (reg & 0x7Fu));
    default: return 0;
    }
}

static const loop_word_t *loop_table(uint8_t code_bits, uint8_t kind, uint8_t *len)
{
    if (code_bits < 13 || code_bits > 15)
        return NULL;
    if (kind == CALIB_KIND_BG) {
        *len = CALIB_BG_LEN;
        return bg_loop[code_bits - 13];
    }
    if (kind == CALIB_KIND_IHRC || kind == CALIB_KIND_ILRC) {
        *len = CALIB_RC_LEN;
        return rc_loop[code_bits - 13];
    }
    return NULL;
}

uint8_t calib_loop_words(uint8_t code_bits, const calib_site_t *site, uint8_t imm, uint16_t *out)
{
    uint8_t len;
    const loop_word_t *t = loop_table(code_bits, site->kind, &len);
    if (!t)
        return 0;
    for (unsigned i = 0; i < len; i++) {
        uint16_t w = t[i].op;
        if (t[i].fl & LW_REG0)
            w |= site->reg0;
        if (t[i].fl & LW_REG1)
            w |= site->reg1;
        if (t[i].fl & LW_REG2)
            w |= site->reg2;
        if (t[i].fl & LW_FIXUP)
            w = (uint16_t)(w + site->addr);
        if (t[i].fl & LW_RESULT)
            w = (uint16_t)((w & 0xFF00u) | imm);
        out[i] = w;
    }
    return len;
}

uint8_t calib_final_words(uint8_t code_bits, const calib_site_t *site, uint8_t k, uint16_t *out)
{
    uint8_t len;
    const loop_word_t *t = loop_table(code_bits, site->kind, &len);
    if (!t || calib_loop_words(code_bits, site, k, out) != len)
        return 0;
    for (unsigned i = 0; i < len; i++)
        if (t[i].fl & LW_NOP)
            out[i] = 0;
    return len;
}

uint8_t calib_placeholder_words(uint8_t code_bits, const calib_site_t *site, uint16_t *out)
{
    uint16_t and_op = calib_and_opcode(code_bits);
    uint8_t v[CALIB_BG_LEN];
    uint8_t len;
    if (!and_op || !loop_table(code_bits, site->kind, &len))
        return 0;
    if (site->kind == CALIB_KIND_BG) {
        memset(v, 0, sizeof v);
        v[0] = 'B';
        v[1] = 'G';
        v[2] = CALIB_KIND_BG;
        v[3] = (uint8_t)site->mV;
        v[4] = (uint8_t)(site->mV >> 8);
        v[5] = site->reg0;
        v[6] = site->reg1;
        v[7] = site->reg2;
    } else {
        v[0] = 'R';
        v[1] = 'C';
        v[2] = site->kind;
        for (unsigned i = 0; i < 4; i++)
            v[3 + i] = (uint8_t)(site->target_hz >> (8u * i));
        v[7] = (uint8_t)site->mV;
        v[8] = (uint8_t)(site->mV >> 8);
        v[9] = site->reg0;
    }
    for (unsigned i = 0; i < len; i++)
        out[i] = (uint16_t)(and_op | v[i]);
    return len;
}

static bool regs_of(const chip_info_t *chip, uint8_t kind, uint8_t regs[3])
{
    regs[1] = regs[2] = 0;
    switch (kind) {
    case CALIB_KIND_IHRC: regs[0] = chip->reg_ihrcr; break;
    case CALIB_KIND_ILRC: regs[0] = chip->reg_ilrcr; break;
    case CALIB_KIND_BG:
        regs[0] = chip->reg_bgtr;
        regs[1] = chip->reg_gpcc;
        regs[2] = chip->reg_gpcs;
        break;
    default: return false;
    }
    return regs[0] != CHIP_REG_UNKNOWN && regs[1] != CHIP_REG_UNKNOWN && regs[2] != CHIP_REG_UNKNOWN;
}

void calib_site_from_match(const chip_info_t *chip, uint16_t addr, const calib_match_t *m, uint32_t target_hz,
                           uint16_t mV, calib_site_t *site)
{
    uint8_t regs[3];
    regs_of(chip, m->kind, regs);
    site->addr = addr;
    site->kind = m->kind;
    site->len = m->len;
    site->target_hz = m->kind == CALIB_KIND_BG ? 0 : target_hz;
    site->mV = mV;
    site->reg0 = regs[0];
    site->reg1 = regs[1];
    site->reg2 = regs[2];
}

bool calib_match(const chip_info_t *chip, const uint16_t *w, unsigned avail, uint16_t addr, calib_match_t *m)
{
    static const uint8_t kinds[] = {CALIB_KIND_IHRC, CALIB_KIND_ILRC, CALIB_KIND_BG};
    uint16_t ref[CALIB_BG_LEN];
    for (unsigned i = 0; i < sizeof kinds; i++) {
        uint8_t regs[3];
        if (!regs_of(chip, kinds[i], regs))
            continue;
        calib_site_t s = {.addr = addr, .kind = kinds[i], .reg0 = regs[0], .reg1 = regs[1], .reg2 = regs[2]};
        uint8_t len;
        if (!loop_table(chip->code_bits, kinds[i], &len) || avail < len)
            continue;
        uint8_t k = (uint8_t)w[kinds[i] == CALIB_KIND_BG ? BG_K_POS : RC_K_POS];
        calib_final_words(chip->code_bits, &s, k, ref);
        if (!memcmp(w, ref, 2u * len)) {
            *m = (calib_match_t){CALIB_FORM_FOOTPRINT, kinds[i], k, len};
            return true;
        }
        calib_loop_words(chip->code_bits, &s, k, ref);
        if (!memcmp(w, ref, 2u * len)) {
            *m = (calib_match_t){CALIB_FORM_LOOP, kinds[i], k, len};
            return true;
        }
    }
    return false;
}

uint8_t calib_scan(const chip_info_t *chip, word_get_fn get, void *ctx, uint16_t count, uint16_t *addrs,
                   calib_match_t *matches, uint8_t max)
{
    uint16_t win[CALIB_BG_LEN];
    uint8_t n = 0;
    for (uint32_t a = 0; a < count && n < max;) {
        unsigned avail = 0;
        for (; avail < CALIB_BG_LEN && a + avail < count; avail++) {
            bool def;
            win[avail] = get(ctx, (uint16_t)(a + avail), &def);
        }
        calib_match_t m;
        if (calib_match(chip, win, avail, (uint16_t)a, &m)) {
            addrs[n] = (uint16_t)a;
            matches[n++] = m;
            a += m.len;
        } else {
            a++;
        }
    }
    return n;
}
