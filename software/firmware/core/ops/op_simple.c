/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PROBE, ERASE, BLANK_CHECK and VERIFY (contracts/protocol.md §4; VERIFY with placeholders, FR-047). */
#include <string.h>

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "op_manager.h"
#include "op_pkg.h"
#include "package/image_crc.h"
#include "proto/proto_defs.h"

static uint16_t mismatch(uint16_t status, uint16_t addr, uint16_t expected, uint16_t actual)
{
    op_result_t *r = op_current();
    r->fail_addr = addr;
    r->expected = expected;
    r->actual = actual;
    return status;
}

static uint16_t probe_expecting(const chip_info_t *chip)
{
    op_result_t *r = op_current();
    op_stage(PDK_STAGE_ID);
    uint16_t id = 0;
    uint8_t family = 0;
    uint16_t st = chip_engine_probe(&id, &family, op_poll);
    if (st != PDK_OK)
        return st;
    r->chip_id_found = id;
    r->family_found = family;
    return chip && id != chip->id12 ? PDK_CHIP_ID_MISMATCH : PDK_OK;
}

/* The chip type of ERASE / BLANK_CHECK: OP_START chip_id12, or 0 = the active package's. */
static uint16_t target_chip(const chip_info_t **chip)
{
    uint16_t id = op_request()->chip_id12;
    if (id) {
        *chip = chip_by_id(id);
        return *chip ? PDK_OK : PDK_PKG_CHIP_UNSUPPORTED;
    }
    uint16_t st = op_pkg_load();
    *chip = op_pkg.chip;
    return st;
}

/* ---------------------------------------------------------------- blank check */

typedef struct {
    const chip_info_t *chip;
    uint16_t status;
} blank_ctx_t;

static void blank_sink(void *ctx, uint16_t addr, uint16_t word)
{
    blank_ctx_t *b = ctx;
    uint16_t mask = chip_word_mask(b->chip);
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / b->chip->code_words));
    if (op_pkg_counted(b->chip, addr) && word != mask && b->status == PDK_OK)
        b->status = mismatch(PDK_CHIP_NOT_BLANK, addr, mask, word);
}

static uint16_t blank_check(const chip_info_t *chip)
{
    op_stage(PDK_STAGE_BLANK_CHECK);
    blank_ctx_t b = {chip, PDK_OK};
    uint16_t st = chip_engine_read(chip, 0, chip->code_words, blank_sink, &b, op_poll);
    return st != PDK_OK ? st : b.status;
}

uint16_t op_probe_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID};
    op_plan(stages, sizeof stages);
    return probe_expecting(NULL);
}

uint16_t op_erase_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_ERASE, PDK_STAGE_BLANK_CHECK};
    op_plan(stages, sizeof stages);
    const chip_info_t *chip;
    uint16_t st = target_chip(&chip);
    if (st != PDK_OK)
        return st;
    if (!chip_is_flash(chip) || !(chip_ops(chip) & PDK_CHIP_OP_ERASE))
        return PDK_OP_UNSUPPORTED;
    if ((st = probe_expecting(chip)) != PDK_OK)
        return st;
    op_stage(PDK_STAGE_ERASE);
    if ((st = chip_engine_erase(chip, op_poll)) != PDK_OK)
        return st;
    return blank_check(chip);
}

uint16_t op_blank_check_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_BLANK_CHECK};
    op_plan(stages, sizeof stages);
    const chip_info_t *chip;
    uint16_t st = target_chip(&chip);
    if (st != PDK_OK)
        return st;
    if ((st = probe_expecting(chip)) != PDK_OK)
        return st;
    return blank_check(chip);
}

/* ---------------------------------------------------------------- VERIFY */

typedef struct {
    uint16_t status;
    uint16_t buf[CALIB_BG_LEN];
    image_crc_t crc;
} verify_ctx_t;

static void verify_site(verify_ctx_t *v, uint8_t i)
{
    op_pkg_t *p = &op_pkg;
    op_result_t *r = op_current();
    const calib_site_t *s = &p->plan.calib[i];
    calib_match_t m;
    if (calib_match(p->chip, v->buf, s->len, s->addr, &m) && m.form == CALIB_FORM_FOOTPRINT && m.kind == s->kind) {
        r->calib[i].value = m.k;   /* any k: the chip's own calibration */
        r->calib[i].site_status = PDK_SITE_OK;
        return;
    }
    uint16_t want[CALIB_BG_LEN];   /* finished form with the k the chip holds, to point at a word */
    calib_final_words(p->chip->code_bits, s, (uint8_t)v->buf[s->kind == CALIB_KIND_BG ? 7 : 2], want);
    unsigned j = 0;
    while (j + 1u < s->len && (uint16_t)(want[j] & p->mask) == v->buf[j])
        j++;
    if (v->status == PDK_OK)
        v->status = mismatch(PDK_VERIFY_FAILED, (uint16_t)(s->addr + j), (uint16_t)(want[j] & p->mask), v->buf[j]);
}

static void verify_serial(verify_ctx_t *v, uint16_t addr0)
{
    op_pkg_t *p = &op_pkg;
    op_result_t *r = op_current();
    uint16_t ret = serial_ret_opcode(p->chip->code_bits);
    for (unsigned j = 0; j < SERIAL_SITE_LEN; j++) {
        if ((v->buf[j] & 0xFF00u) != ret) {
            if (v->status == PDK_OK)
                v->status = mismatch(PDK_VERIFY_FAILED, (uint16_t)(addr0 + j), ret, v->buf[j]);
            return;
        }
    }
    r->serial_valid = 1;
    for (unsigned j = 0; j < SERIAL_SITE_LEN; j++)
        r->serial[j] = (uint8_t)v->buf[j];
}

static void verify_sink(void *ctx, uint16_t addr, uint16_t word)
{
    verify_ctx_t *v = ctx;
    op_pkg_t *p = &op_pkg;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / p->chip->code_words));
    image_crc_feed(&v->crc, word);
    if (!op_pkg_counted(p->chip, addr))
        return;
    eff_class_t c = eff_classify(&p->eff, addr);
    switch (c.kind) {
    case EFF_CALIB:
        v->buf[c.offset] = word;
        if (c.offset == p->plan.calib[c.index].len - 1u)
            verify_site(v, c.index);
        break;
    case EFF_SERIAL:
        v->buf[c.offset] = word;
        if (c.offset == SERIAL_SITE_LEN - 1u)
            verify_serial(v, (uint16_t)(addr - c.offset));
        break;
    default: {
        uint16_t t = eff_word(&p->eff, addr);
        if (word != t && v->status == PDK_OK)
            v->status = mismatch(PDK_VERIFY_FAILED, addr, t, word);
        break;
    }
    }
}

uint16_t op_verify_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_PKG_CHECK, PDK_STAGE_ID, PDK_STAGE_VERIFY};
    op_plan(stages, sizeof stages);
    op_result_t *r = op_current();
    op_stage(PDK_STAGE_PKG_CHECK);
    uint16_t st = op_pkg_load();
    if (st != PDK_OK)
        return st;
    op_pkg_t *p = &op_pkg;
    r->calib_count = p->plan.calib_count;
    for (uint8_t i = 0; i < p->plan.calib_count; i++) {
        r->calib[i].kind = p->plan.calib[i].kind;
        r->calib[i].target_hz = p->plan.calib[i].target_hz;
        r->calib[i].site_status = PDK_SITE_FAILED;
    }
    if ((st = probe_expecting(p->chip)) != PDK_OK)
        return st;
    op_stage(PDK_STAGE_VERIFY);
    verify_ctx_t v = {.status = PDK_OK};
    image_crc_init(&v.crc, p->chip);
    if ((st = chip_engine_read(p->chip, 0, p->chip->code_words, verify_sink, &v, op_poll)) != PDK_OK)
        return st;
    image_crc_finish(&v.crc, &r->image_crc_raw, &r->image_crc_norm);
    return v.status;
}
