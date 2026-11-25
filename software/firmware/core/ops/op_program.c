/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PROGRAM (data-model ProgramJob, contracts/protocol.md §4): PKG_CHECK → ID → ERASE (FLASH) →
 * BLANK_CHECK (OTP: blank or compatible → retry) → WRITE (package CRC streamed, FR-049) → VERIFY →
 * CALIBRATE each placeholder (FR-043, FR-055) → PKG_CHECK → FUSE → FINAL_VERIFY (FR-048). */
#include <string.h>

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "op_manager.h"
#include "package/effective_image.h"
#include "package/header.h"
#include "package/image_crc.h"
#include "package/serial_rng.h"
#include "package/validate.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"
#include "storage/slots.h"

static struct {
    const chip_info_t *chip;
    pkg_header_t hdr;
    pkg_plan_t plan;
    pkg_slot_reader_t reader;
    eff_image_t eff;
    uint16_t mask;
} p;

static bool counted(uint16_t addr)
{
    return !(p.chip->excl_first && addr == 0) && eff_classify(&p.eff, addr).kind != EFF_RESERVED;
}

static uint16_t mismatch(uint16_t status, uint16_t addr, uint16_t expected, uint16_t actual)
{
    op_result_t *r = op_current();
    r->fail_addr = addr;
    r->expected = expected;
    r->actual = actual;
    return status;
}

/* ---------------------------------------------------------------- PKG_CHECK */

static uint16_t load_package(void)
{
    if (package_store_state() == PDK_PKG_EMPTY)
        return PDK_STORE_EMPTY;
    uint32_t crc;
    if (package_store_recheck(&crc) != PDK_PKG_VALID)
        return PDK_STORE_CORRUPTED;
    uint8_t raw[PKG_HEADER_SIZE];
    package_store_read(0, raw, sizeof raw);
    if (pkg_header_parse(raw, false, slots_size(), &p.hdr, &p.chip) != PDK_OK)
        return PDK_STORE_CORRUPTED;
    pkg_slot_reader_init(&p.reader, package_store_active_slot(), p.hdr.image_words);
    uint16_t bad;
    if (pkg_validate(&p.hdr, p.chip, pkg_slot_word, &p.reader, &p.plan, &bad) != PDK_OK)
        return PDK_STORE_CORRUPTED;
    eff_init(&p.eff, p.chip, &p.hdr, &p.plan, pkg_slot_word, &p.reader);
    p.mask = chip_word_mask(p.chip);
    return PDK_OK;
}

/* ---------------------------------------------------------------- BLANK_CHECK */

typedef struct {
    bool strict;               /* FLASH: everything blank */
    uint16_t status;           /* first incompatibility */
    bool need_write, written;  /* something left to write / something already written */
    bool serial_new;           /* every serial site blank: a new number is needed */
    bool serial_kept;
    uint8_t serial[EFF_SERIAL_BYTES];
    uint16_t buf[CALIB_BG_LEN];
} blank_t;

static void fail_once(blank_t *b, uint16_t addr, uint16_t expected, uint16_t actual)
{
    if (b->status == PDK_OK)
        b->status = mismatch(PDK_CHIP_NOT_BLANK, addr, expected, actual);
}

static void check_calib_site(blank_t *b, uint8_t i)
{
    const calib_site_t *s = &p.plan.calib[i];
    calib_match_t m;
    if (calib_match(p.chip, b->buf, s->len, s->addr, &m) && m.form == CALIB_FORM_FOOTPRINT && m.kind == s->kind) {
        eff_set_final(&p.eff, i, m.k);   /* already calibrated: keep its k */
        b->written = true;
        return;
    }
    uint16_t loop[CALIB_BG_LEN];
    calib_loop_words(p.chip->code_bits, s, CALIB_LOOP_IMM, loop);
    for (unsigned j = 0; j < s->len; j++) {
        uint16_t t = (uint16_t)(loop[j] & p.mask);
        if ((b->buf[j] & t) != t) {
            fail_once(b, (uint16_t)(s->addr + j), t, b->buf[j]);
            return;
        }
        b->need_write |= b->buf[j] != t;
        b->written |= b->buf[j] != p.mask;
    }
}

static void check_serial_site(blank_t *b, uint16_t addr0)
{
    uint16_t ret = serial_ret_opcode(p.chip->code_bits);
    bool blank = b->buf[0] == p.mask;
    for (unsigned j = 0; j < SERIAL_SITE_LEN; j++) {
        bool ok = blank ? b->buf[j] == p.mask : (b->buf[j] & 0xFF00u) == ret;
        if (!ok) {
            fail_once(b, (uint16_t)(addr0 + j), blank ? p.mask : ret, b->buf[j]);
            return;
        }
    }
    if (blank) {
        b->serial_new = true;
        b->need_write = true;
        return;
    }
    for (unsigned j = 0; j < SERIAL_SITE_LEN; j++)
        b->serial[j] = (uint8_t)b->buf[j];
    b->serial_kept = true;
    b->written = true;
}

static void blank_sink(void *ctx, uint16_t addr, uint16_t word)
{
    blank_t *b = ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / p.chip->code_words));
    if (!counted(addr))
        return;
    if (b->strict) {
        if (word != p.mask)
            fail_once(b, addr, p.mask, word);
        return;
    }
    eff_class_t c = eff_classify(&p.eff, addr);
    switch (c.kind) {
    case EFF_CALIB:
        b->buf[c.offset] = word;
        if (c.offset == p.plan.calib[c.index].len - 1u)
            check_calib_site(b, c.index);
        break;
    case EFF_SERIAL:
        b->buf[c.offset] = word;
        if (c.offset == SERIAL_SITE_LEN - 1u)
            check_serial_site(b, (uint16_t)(addr - c.offset));
        break;
    default: {
        uint16_t t = eff_word(&p.eff, addr);   /* (content AND target) == target */
        if ((word & t) != t)
            fail_once(b, addr, t, word);
        b->need_write |= word != t;
        b->written |= word != p.mask;
        break;
    }
    }
}

/* ---------------------------------------------------------------- WRITE helpers */

/* Writes the words of [first, last] that belong to the image, whole blocks, blank elsewhere. */
static uint16_t write_range(uint16_t first, uint16_t last, bool with_fuse)
{
    uint8_t bs = p.chip->write_block ? p.chip->write_block : 1u;
    uint16_t words[16];
    for (uint32_t a = (uint32_t)(first - first % bs); a <= last; a += bs) {
        bool any = false;
        for (unsigned i = 0; i < bs; i++) {
            uint16_t addr = (uint16_t)(a + i);
            eff_kind_t k = addr < p.chip->code_words ? eff_classify(&p.eff, addr).kind : EFF_RESERVED;
            bool take = addr >= first && addr <= last &&
                        (k == EFF_PLAIN || k == EFF_CALIB || k == EFF_SERIAL || (k == EFF_FUSE && with_fuse));
            words[i] = take ? eff_word(&p.eff, addr) : p.mask;
            any |= words[i] != p.mask;
        }
        if (any) {
            uint16_t st = chip_engine_write_block(p.chip, (uint16_t)a, words, bs, op_poll);
            if (st != PDK_OK)
                return st;
        }
        op_percent((uint8_t)(a * 100u / p.chip->code_words));
    }
    return PDK_OK;
}

/* ---------------------------------------------------------------- VERIFY / FINAL_VERIFY */

typedef struct {
    bool with_fuse;
    uint16_t status;
    image_crc_t crc;
} verify_t;

static void verify_sink(void *ctx, uint16_t addr, uint16_t word)
{
    verify_t *v = ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / p.chip->code_words));
    image_crc_feed(&v->crc, word);
    if (!counted(addr) || (!v->with_fuse && addr == chip_fuse_addr(p.chip)))
        return;
    uint16_t t = eff_word(&p.eff, addr);
    if (word != t && v->status == PDK_OK)
        v->status = mismatch(PDK_VERIFY_FAILED, addr, t, word);
}

static uint16_t verify(bool final)
{
    verify_t v = {.with_fuse = final, .status = PDK_OK};
    image_crc_init(&v.crc, p.chip);
    uint16_t st = chip_engine_read(p.chip, 0, p.chip->code_words, verify_sink, &v, op_poll);
    if (st != PDK_OK)
        return st;
    if (final) {
        op_result_t *r = op_current();
        image_crc_finish(&v.crc, &r->image_crc_raw, &r->image_crc_norm);
    }
    return v.status;
}

/* ---------------------------------------------------------------- the pipeline */

uint16_t op_program_run(void)
{
    op_result_t *r = op_current();
    const op_request_t *q = op_request();
    op_stage(PDK_STAGE_PKG_CHECK);
    uint16_t st = load_package();
    if (st != PDK_OK)
        return st;

    bool flash = chip_is_flash(p.chip);
    uint32_t pf = p.hdr.flags;
    bool skip_erase = !flash || (pf & PKG_F_SKIP_ERASE) || (q->op_flags & PDK_OPF_SKIP_ERASE);
    bool skip_blank = (pf & PKG_F_SKIP_BLANK_CHECK) || (q->op_flags & PDK_OPF_SKIP_BLANK_CHECK);
    bool skip_verify = (pf & PKG_F_SKIP_VERIFY) || (q->op_flags & PDK_OPF_SKIP_VERIFY);
    bool finalize = (q->op_flags & PDK_OPF_CALIB_FINALIZE_ON_FAIL) && q->source == PDK_SOURCE_HOST;
    if (!flash && skip_blank)
        return PDK_PKG_OTP_BLANK_CHECK_REQUIRED;

    uint8_t stages[10], n = 0;
    stages[n++] = PDK_STAGE_PKG_CHECK;
    stages[n++] = PDK_STAGE_ID;
    if (!skip_erase)
        stages[n++] = PDK_STAGE_ERASE;
    if (!skip_blank)
        stages[n++] = PDK_STAGE_BLANK_CHECK;
    stages[n++] = PDK_STAGE_WRITE;
    if (!skip_verify)
        stages[n++] = PDK_STAGE_VERIFY;
    if (p.plan.calib_count)
        stages[n++] = PDK_STAGE_CALIBRATE;
    stages[n++] = PDK_STAGE_PKG_CHECK;
    if (p.plan.fuse_defined)
        stages[n++] = PDK_STAGE_FUSE;
    stages[n++] = PDK_STAGE_FINAL_VERIFY;
    op_plan(stages, n);

    r->calib_count = p.plan.calib_count;
    for (uint8_t i = 0; i < p.plan.calib_count; i++) {
        r->calib[i].kind = p.plan.calib[i].kind;
        r->calib[i].target_hz = p.plan.calib[i].target_hz;
        r->calib[i].site_status = PDK_SITE_FAILED;   /* until it is finished */
    }

    /* ID */
    op_stage(PDK_STAGE_ID);
    uint16_t id = 0;
    uint8_t family = 0;
    st = chip_engine_probe(&id, &family, op_poll);
    if (st != PDK_OK)
        return st;
    r->chip_id_found = id;
    r->family_found = family;
    if (id != p.chip->id12)
        return PDK_CHIP_ID_MISMATCH;

    if (!skip_erase) {
        op_stage(PDK_STAGE_ERASE);
        if ((st = chip_engine_erase(p.chip, op_poll)) != PDK_OK)
            return st;
    }

    blank_t b = {.strict = flash, .status = PDK_OK, .serial_new = false};
    if (!skip_blank) {
        op_stage(PDK_STAGE_BLANK_CHECK);
        if ((st = chip_engine_read(p.chip, 0, p.chip->code_words, blank_sink, &b, op_poll)) != PDK_OK)
            return st;
        if (b.status != PDK_OK)
            return b.status;
    }
    if (flash || skip_blank)
        b.serial_new = p.plan.serial_count != 0;
    bool resumed = !flash && b.written;
    if (!flash && !b.need_write)
        return PDK_CHIP_ALREADY_PROGRAMMED;
    if (resumed)
        r->warnings |= PDK_WARN_RESUMED;

    /* serial number: kept from the chip, or a new one spent in the journal before it is written */
    if (b.serial_kept) {
        eff_set_serial(&p.eff, b.serial);
    } else if (b.serial_new) {
        uint32_t seq;
        if (!package_store_bump(&seq))
            return PDK_STORE_WRITE_FAILED;
        if ((st = serial_rng_next(seq, b.serial)) != PDK_OK)
            return st;
        eff_set_serial(&p.eff, b.serial);
    }
    if (b.serial_kept || b.serial_new) {
        r->serial_valid = 1;
        memcpy(r->serial, b.serial, EFF_SERIAL_BYTES);
    }

    /* WRITE, re-checking the package as it is read (FR-049) */
    op_stage(PDK_STAGE_WRITE);
    pkg_slot_crc_begin(&p.reader, p.hdr.package_size);
    if ((st = write_range(0, (uint16_t)(p.chip->code_words - 1u), false)) != PDK_OK)
        return st;
    if (!pkg_slot_crc_end(&p.reader))
        return PDK_STORE_CORRUPTED;

    if (!skip_verify) {
        op_stage(PDK_STAGE_VERIFY);
        if ((st = verify(false)) != PDK_OK)
            return st;
    }

    uint16_t final_status = PDK_OK;
    if (p.plan.calib_count) {
        op_stage(PDK_STAGE_CALIBRATE);
        for (uint8_t i = 0; i < p.plan.calib_count; i++) {
            const calib_site_t *s = &p.plan.calib[i];
            op_percent((uint8_t)(i * 100u / p.plan.calib_count));
            if (p.eff.final_mask & (1u << i)) {   /* finished on an earlier attempt */
                r->calib[i].value = p.eff.value[i];
                r->calib[i].site_status = PDK_SITE_OK;
                continue;
            }
            uint8_t value = 0;
            uint32_t hz = 0;
            st = chip_engine_calibrate(p.chip, s->addr, s->kind, s->target_hz, s->mV, &value, &hz, op_poll);
            r->calib[i].value = value;
            r->calib[i].measured_hz = hz;
            if (st == PDK_CALIB_FAILED && finalize) {
                r->calib[i].site_status = PDK_SITE_FORCED;
                r->warnings |= PDK_WARN_CALIB_FORCED;
                final_status = PDK_CALIB_FAILED;
            } else if (st != PDK_OK) {
                return st;   /* nothing more is written: the loop stays for a later retry */
            } else {
                r->calib[i].site_status = PDK_SITE_OK;
            }
            eff_set_final(&p.eff, i, value);
            if ((st = write_range(s->addr, (uint16_t)(s->addr + s->len - 1u), false)) != PDK_OK)
                return st;
        }
    }

    op_stage(PDK_STAGE_PKG_CHECK);
    uint32_t crc;
    if (package_store_recheck(&crc) != PDK_PKG_VALID)
        return PDK_STORE_CORRUPTED;

    if (p.plan.fuse_defined) {
        op_stage(PDK_STAGE_FUSE);
        uint16_t fuse_addr = chip_fuse_addr(p.chip);
        if ((st = write_range(fuse_addr, fuse_addr, true)) != PDK_OK)
            return st;
        r->fuse_written = p.plan.fuse;
    }

    op_stage(PDK_STAGE_FINAL_VERIFY);
    if ((st = verify(true)) != PDK_OK)
        return st;
    return final_status;
}
