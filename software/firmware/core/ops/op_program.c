/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PROGRAM (data-model ProgramJob, contracts/protocol.md §4): PKG_CHECK → ID → ERASE (FLASH) →
 * BLANK_CHECK (OTP: blank or compatible → retry) → WRITE (package CRC streamed, FR-049) → VERIFY →
 * CALIBRATE each placeholder (FR-043, FR-055) → PKG_CHECK → FUSE → FINAL_VERIFY (FR-048). */
#include <string.h>

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "op_manager.h"
#include "op_pkg.h"
#include "package/effective_image.h"
#include "package/image_crc.h"
#include "package/serial_rng.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"

static op_pkg_t *const pk = &op_pkg;

static bool counted(uint16_t addr) { return op_pkg_counted(pk->chip, addr); }

static uint16_t mismatch(uint16_t status, uint16_t addr, uint16_t expected, uint16_t actual)
{
    op_result_t *r = op_current();
    r->fail_addr = addr;
    r->expected = expected;
    r->actual = actual;
    return status;
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
    const calib_site_t *s = &pk->plan.calib[i];
    calib_match_t m;
    if (calib_match(pk->chip, b->buf, s->len, s->addr, &m) && m.form == CALIB_FORM_FOOTPRINT && m.kind == s->kind) {
        eff_set_final(&pk->eff, i, m.k);   /* already calibrated: keep its k */
        b->written = true;
        return;
    }
    uint16_t loop[CALIB_BG_LEN];
    calib_loop_words(pk->chip->code_bits, s, CALIB_LOOP_IMM, loop);
    for (unsigned j = 0; j < s->len; j++) {
        uint16_t t = (uint16_t)(loop[j] & pk->mask);
        if ((b->buf[j] & t) != t) {
            fail_once(b, (uint16_t)(s->addr + j), t, b->buf[j]);
            return;
        }
        b->need_write |= b->buf[j] != t;
        b->written |= b->buf[j] != pk->mask;
    }
}

static void check_serial_site(blank_t *b, uint16_t addr0)
{
    uint16_t ret = serial_ret_opcode(pk->chip->code_bits);
    bool blank = b->buf[0] == pk->mask;
    for (unsigned j = 0; j < SERIAL_SITE_LEN; j++) {
        bool ok = blank ? b->buf[j] == pk->mask : (b->buf[j] & 0xFF00u) == ret;
        if (!ok) {
            fail_once(b, (uint16_t)(addr0 + j), blank ? pk->mask : ret, b->buf[j]);
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
        op_percent((uint8_t)((uint32_t)addr * 100u / pk->chip->code_words));
    if (!counted(addr))
        return;
    if (b->strict) {
        if (word != pk->mask)
            fail_once(b, addr, pk->mask, word);
        return;
    }
    eff_class_t c = eff_classify(&pk->eff, addr);
    switch (c.kind) {
    case EFF_CALIB:
        b->buf[c.offset] = word;
        if (c.offset == pk->plan.calib[c.index].len - 1u)
            check_calib_site(b, c.index);
        break;
    case EFF_SERIAL:
        b->buf[c.offset] = word;
        if (c.offset == SERIAL_SITE_LEN - 1u)
            check_serial_site(b, (uint16_t)(addr - c.offset));
        break;
    default: {
        uint16_t t = eff_word(&pk->eff, addr);   /* (content AND target) == target */
        if ((word & t) != t)
            fail_once(b, addr, t, word);
        b->need_write |= word != t;
        b->written |= word != pk->mask;
        break;
    }
    }
}

/* ---------------------------------------------------------------- WRITE helpers */

/* Writes the words of [first, last] that belong to the image, whole blocks, blank elsewhere. */
static uint16_t write_range(uint16_t first, uint16_t last, bool with_fuse)
{
    uint8_t bs = pk->chip->write_block ? pk->chip->write_block : 1u;
    uint16_t words[16];
    for (uint32_t a = (uint32_t)(first - first % bs); a <= last; a += bs) {
        bool any = false;
        for (unsigned i = 0; i < bs; i++) {
            uint16_t addr = (uint16_t)(a + i);
            eff_kind_t k = addr < pk->chip->code_words ? eff_classify(&pk->eff, addr).kind : EFF_RESERVED;
            bool take = addr >= first && addr <= last &&
                        (k == EFF_PLAIN || k == EFF_CALIB || k == EFF_SERIAL || (k == EFF_FUSE && with_fuse));
            words[i] = take ? eff_word(&pk->eff, addr) : pk->mask;
            any |= words[i] != pk->mask;
        }
        if (any) {
            uint16_t st = chip_engine_write_block(pk->chip, (uint16_t)a, words, bs, op_poll);
            if (st != PDK_OK)
                return st;
        }
        op_percent((uint8_t)(a * 100u / pk->chip->code_words));
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
        op_percent((uint8_t)((uint32_t)addr * 100u / pk->chip->code_words));
    image_crc_feed(&v->crc, word);
    if (!counted(addr) || (!v->with_fuse && addr == chip_fuse_addr(pk->chip)))
        return;
    uint16_t t = eff_word(&pk->eff, addr);
    if (word != t && v->status == PDK_OK)
        v->status = mismatch(PDK_VERIFY_FAILED, addr, t, word);
}

static uint16_t verify(bool final)
{
    verify_t v = {.with_fuse = final, .status = PDK_OK};
    image_crc_init(&v.crc, pk->chip);
    uint16_t st = chip_engine_read(pk->chip, 0, pk->chip->code_words, verify_sink, &v, op_poll);
    if (st != PDK_OK)
        return st;
    if (final) {
        op_result_t *r = op_current();
        image_crc_finish(&v.crc, &r->image_crc_raw, &r->image_crc_norm);
    }
    return v.status;
}

/* SNAP_STANDARD (research R9): the engine measured the copy at the master's value and calibrated to the
 * nearest standard SYSCLK if that was within 10 %; otherwise the master's value stayed. */
static void snap_result(op_calib_result_t *e, uint8_t *warnings, uint32_t hz)
{
    static const uint32_t standard[] = {16000000, 8000000, 4000000, 2000000, 1000000, 500000};
    uint32_t std = standard[0];
    for (unsigned i = 1; i < sizeof standard / sizeof standard[0]; i++)
        if ((hz > standard[i] ? hz - standard[i] : standard[i] - hz) < (hz > std ? hz - std : std - hz))
            std = standard[i];
    if ((hz > std ? hz - std : std - hz) <= std / 10u) {
        e->target_hz = std;
        e->site_status = PDK_SITE_OK;
        *warnings |= PDK_WARN_CALIB_SNAPPED;
    } else {
        e->target_hz = 0;
        e->site_status = PDK_SITE_COPIED;
        *warnings |= PDK_WARN_CALIB_VALUE_COPIED;
    }
}

/* ---------------------------------------------------------------- the pipeline */

uint16_t op_program_run(void)
{
    op_result_t *r = op_current();
    const op_request_t *q = op_request();
    op_stage(PDK_STAGE_PKG_CHECK);
    uint16_t st = op_pkg_load();
    if (st != PDK_OK)
        return st;

    bool flash = chip_is_flash(pk->chip);
    uint32_t pf = pk->hdr.flags;
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
    if (pk->plan.calib_count)
        stages[n++] = PDK_STAGE_CALIBRATE;
    stages[n++] = PDK_STAGE_PKG_CHECK;
    if (pk->plan.fuse_defined)
        stages[n++] = PDK_STAGE_FUSE;
    stages[n++] = PDK_STAGE_FINAL_VERIFY;
    op_plan(stages, n);

    r->calib_count = pk->plan.calib_count;
    for (uint8_t i = 0; i < pk->plan.calib_count; i++) {
        r->calib[i].kind = pk->plan.calib[i].kind;
        r->calib[i].target_hz = pk->plan.calib[i].target_hz;
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
    if (id != pk->chip->id12)
        return PDK_CHIP_ID_MISMATCH;

    if (!skip_erase) {
        op_stage(PDK_STAGE_ERASE);
        if ((st = chip_engine_erase(pk->chip, op_poll)) != PDK_OK)
            return st;
    }

    blank_t b = {.strict = flash, .status = PDK_OK, .serial_new = false};
    if (!skip_blank) {
        op_stage(PDK_STAGE_BLANK_CHECK);
        if ((st = chip_engine_read(pk->chip, 0, pk->chip->code_words, blank_sink, &b, op_poll)) != PDK_OK)
            return st;
        if (b.status != PDK_OK)
            return b.status;
    }
    if (flash || skip_blank)
        b.serial_new = pk->plan.serial_count != 0;
    bool resumed = !flash && b.written;
    if (!flash && !b.need_write)
        return PDK_CHIP_ALREADY_PROGRAMMED;
    if (resumed)
        r->warnings |= PDK_WARN_RESUMED;

    /* serial number: kept from the chip, or a new one spent in the journal before it is written */
    if (b.serial_kept) {
        eff_set_serial(&pk->eff, b.serial);
    } else if (b.serial_new) {
        uint32_t seq;
        if (!package_store_bump(&seq))
            return PDK_STORE_WRITE_FAILED;
        if ((st = serial_rng_next(seq, b.serial)) != PDK_OK)
            return st;
        eff_set_serial(&pk->eff, b.serial);
    }
    if (b.serial_kept || b.serial_new) {
        r->serial_valid = 1;
        memcpy(r->serial, b.serial, EFF_SERIAL_BYTES);
    }

    /* WRITE, re-checking the package as it is read (FR-049) */
    op_stage(PDK_STAGE_WRITE);
    pkg_slot_crc_begin(&pk->reader, pk->hdr.package_size);
    if ((st = write_range(0, (uint16_t)(pk->chip->code_words - 1u), false)) != PDK_OK)
        return st;
    if (!pkg_slot_crc_end(&pk->reader))
        return PDK_STORE_CORRUPTED;

    if (!skip_verify) {
        op_stage(PDK_STAGE_VERIFY);
        if ((st = verify(false)) != PDK_OK)
            return st;
    }

    uint16_t final_status = PDK_OK;
    if (pk->plan.calib_count) {
        op_stage(PDK_STAGE_CALIBRATE);
        for (uint8_t i = 0; i < pk->plan.calib_count; i++) {
            const calib_site_t *s = &pk->plan.calib[i];
            op_percent((uint8_t)(i * 100u / pk->plan.calib_count));
            if (pk->eff.final_mask & (1u << i)) {   /* finished on an earlier attempt */
                r->calib[i].value = pk->eff.value[i];
                r->calib[i].site_status = PDK_SITE_OK;
                continue;
            }
            uint8_t value = 0;
            uint32_t hz = 0;
            st = chip_engine_calibrate(pk->chip, s->addr, s->kind, s->target_hz, s->mV, &value, &hz, op_poll);
            r->calib[i].value = value;
            r->calib[i].measured_hz = hz;
            if (st == PDK_CALIB_FAILED && finalize) {
                r->calib[i].site_status = PDK_SITE_FORCED;
                r->warnings |= PDK_WARN_CALIB_FORCED;
                final_status = PDK_CALIB_FAILED;
            } else if (st != PDK_OK) {
                return st;   /* nothing more is written: the loop stays for a later retry */
            } else if (calib_is_snap(s->target_hz)) {
                snap_result(&r->calib[i], &r->warnings, hz);
            } else {
                r->calib[i].site_status = PDK_SITE_OK;
            }
            eff_set_final(&pk->eff, i, value);
            if ((st = write_range(s->addr, (uint16_t)(s->addr + s->len - 1u), false)) != PDK_OK)
                return st;
        }
    }

    op_stage(PDK_STAGE_PKG_CHECK);
    uint32_t crc;
    if (package_store_recheck(&crc) != PDK_PKG_VALID)
        return PDK_STORE_CORRUPTED;

    if (pk->plan.fuse_defined) {
        op_stage(PDK_STAGE_FUSE);
        uint16_t fuse_addr = chip_fuse_addr(pk->chip);
        if ((st = write_range(fuse_addr, fuse_addr, true)) != PDK_OK)
            return st;
        r->fuse_written = pk->plan.fuse;
    }

    op_stage(PDK_STAGE_FINAL_VERIFY);
    if ((st = verify(true)) != PDK_OK)
        return st;
    return final_status;
}
