/* SPDX-License-Identifier: GPL-3.0-or-later */
/* COPY (contracts/protocol.md §4, research R9, data-model CopyJob): ID → READ → STORE. The new package is
 * built while the master is read, straight into the inactive slot: words through a 16-word window in
 * which finished calibration footprints become free-pdk placeholders by the mode of their kind
 * (KEEP_VALUE / TARGET_FROM_HOST / SNAP_STANDARD for IHRC and ILRC, restore or keep for BandGap), the
 * header is written last (its name holds the master's normalized checksum), then the package goes through
 * the PKG_COMMIT checks and becomes the active one. MEASURE_MASTER comes with RESTORE_MASTER (part 7c).
 * The master's serial number is not recognised (FR-052); a trim register written outside a footprint is
 * copied as code with a warning (FR-053); a measurement loop in the master needs ALLOW_MASTER_LOOP and a
 * target from the PC (FR-051). */
#include <stdio.h>
#include <string.h>

#include "chips/calib_footprint.h"
#include "chips/serial_sites.h"
#include "op_manager.h"
#include "op_pkg.h"
#include "op_read.h"
#include "package/header.h"
#include "package/image_crc.h"
#include "package/pkg_cmds.h"
#include "proto/crc32.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/journal.h"
#include "storage/package_store.h"
#include "storage/slots.h"
#include "chip_engine.h"

enum { MODE_KEEP = 0, MODE_TARGET = 1, MODE_SNAP = 2, MODE_MEASURE = 3 };
enum { COPY_ALLOW_MODIFY_MASTER = 1u << 0, COPY_ALLOW_MASTER_LOOP = 1u << 1 };

#define COPY_DEFAULT_MV 5000u
#define BG_TARGET_MV 3950u
#define WINDOW 16u

typedef struct {
    uint8_t mode_ihrc, mode_ilrc, bg_restore, flags;
    uint32_t target_ihrc, target_ilrc;
    uint16_t mV;
    char name[33];
} copy_req_t;

static struct {
    const chip_info_t *chip;
    copy_req_t q;
    uint16_t mask, ret_op, mov_a;
    uint8_t slot;
    uint32_t data_crc;
    image_crc_t sums;
    uint16_t win[WINDOW];
    uint8_t n;
    uint16_t base;               /* address of win[0] */
    uint8_t out[64];             /* words on their way to the slot */
    uint8_t out_n;
    uint32_t out_off;
    bool write_failed;
    bool prev_mov;               /* the last plain word was MOV A,#k */
    uint16_t prev_addr;
    uint8_t ret_run;             /* RET #imm words in a row */
    bool serial_seen;
    uint8_t loops;
    uint16_t first_loop;
    bool loop_refused;           /* a loop with ALLOW_MASTER_LOOP but no target for its kind */
    uint16_t last;               /* the fuse word: read last */
} c;

/* ---------------------------------------------------------------- request */

static uint16_t parse_request(void)
{
    const op_request_t *r = op_request();
    copy_req_t *q = &c.q;
    memset(q, 0, sizeof *q);
    if (r->source == PDK_SOURCE_LOCAL) {   /* local COPY: the stored configuration (CONFIG_SET) */
        const journal_state_t *j = journal_get();
        q->mode_ihrc = j->cfg_copy_ihrc;
        q->mode_ilrc = j->cfg_copy_ilrc;
        q->bg_restore = j->cfg_copy_bg;
    } else if (r->ext_len) {
        if (r->ext_len != 14 && r->ext_len != 46)
            return PDK_BAD_PAYLOAD;
        q->mode_ihrc = r->ext[0];
        q->mode_ilrc = r->ext[1];
        q->bg_restore = r->ext[2];
        q->flags = r->ext[3];
        q->target_ihrc = le32(r->ext + 4);
        q->target_ilrc = le32(r->ext + 8);
        q->mV = le16(r->ext + 12);
        if (r->ext_len == 46) {
            memcpy(q->name, r->ext + 14, 32);
            for (unsigned i = 0; q->name[i]; i++)
                if ((uint8_t)q->name[i] < 0x20)
                    return PDK_BAD_PAYLOAD;
        }
    }
    if (q->mode_ihrc > MODE_MEASURE || q->mode_ilrc > MODE_MEASURE || q->mode_ilrc == MODE_SNAP ||
        q->bg_restore > 1 || (q->flags & ~(COPY_ALLOW_MODIFY_MASTER | COPY_ALLOW_MASTER_LOOP)))
        return PDK_BAD_PAYLOAD;
    if ((q->mode_ihrc == MODE_TARGET && !q->target_ihrc) || (q->mode_ilrc == MODE_TARGET && !q->target_ilrc))
        return PDK_BAD_PAYLOAD;
    if ((q->mode_ihrc == MODE_MEASURE || q->mode_ilrc == MODE_MEASURE) && !(q->flags & COPY_ALLOW_MODIFY_MASTER))
        return PDK_MASTER_MODIFY_NOT_ALLOWED;
    if (!q->mV)
        q->mV = COPY_DEFAULT_MV;
    return PDK_OK;
}

/* ---------------------------------------------------------------- building the package */

static void out_flush(void)
{
    if (c.out_n && !slots_write(c.slot, c.out_off, c.out, c.out_n))
        c.write_failed = true;
    c.out_off += c.out_n;
    c.out_n = 0;
}

static void out_word(uint16_t addr, uint16_t w)
{
    if (chip_addr_reserved(c.chip, addr))
        w = 0xFFFF;   /* not defined in the package */
    put16(c.out + c.out_n, w);
    c.out_n += 2;
    if (c.out_n == sizeof c.out)
        out_flush();
}

static void add_result(uint8_t kind, uint8_t master, uint32_t target, uint8_t mode, uint8_t status)
{
    op_result_t *r = op_current();
    if (r->calib_count >= CALIB_MAX_SITES)
        return;
    op_calib_result_t *e = &r->calib[r->calib_count++];
    e->kind = kind;
    e->master_value = master;
    e->value = master;
    e->target_hz = target;
    e->mode = mode;
    e->site_status = status;
}

/* A finished footprint at the head of the window: the placeholder its mode asks for, or the words as they are. */
static void footprint(const calib_match_t *m, uint16_t *words)
{
    op_result_t *r = op_current();
    calib_site_t s;
    uint8_t mode = m->kind == CALIB_KIND_IHRC ? c.q.mode_ihrc : m->kind == CALIB_KIND_ILRC ? c.q.mode_ilrc
                                                                                            : (c.q.bg_restore ? MODE_TARGET : MODE_KEEP);
    uint32_t target = 0;
    if (m->kind == CALIB_KIND_BG) {
        calib_site_from_match(c.chip, c.base, m, 0, BG_TARGET_MV, &s);
    } else {
        target = mode == MODE_SNAP ? (CALIB_SNAP_TAG | m->k)
                                   : (m->kind == CALIB_KIND_IHRC ? c.q.target_ihrc : c.q.target_ilrc);
        calib_site_from_match(c.chip, c.base, m, target, c.q.mV, &s);
    }
    if (mode == MODE_KEEP || r->calib_count >= CALIB_MAX_SITES) {
        add_result(m->kind, m->k, 0, MODE_KEEP, PDK_SITE_COPIED);
        r->warnings |= PDK_WARN_CALIB_VALUE_COPIED;
        return;   /* the master's k goes into the copies as it is */
    }
    calib_placeholder_words(c.chip->code_bits, &s, words);
    add_result(m->kind, m->k, m->kind == CALIB_KIND_BG ? 0 : target, mode, PDK_SITE_OK);
}

/* A measurement loop in the master (FR-051): with ALLOW_MASTER_LOOP the placeholder gets the target from
 * the PC, which its kind's mode must be. */
static void master_loop(const calib_match_t *m, uint16_t *words)
{
    if (!c.loops++)
        c.first_loop = c.base;
    uint8_t mode = m->kind == CALIB_KIND_IHRC ? c.q.mode_ihrc : m->kind == CALIB_KIND_ILRC ? c.q.mode_ilrc : MODE_KEEP;
    if (!(c.q.flags & COPY_ALLOW_MASTER_LOOP)) {
        add_result(m->kind, m->k, 0, mode, PDK_SITE_MASTER_LOOP);
        return;
    }
    if (mode != MODE_TARGET) {
        c.loop_refused = true;
        return;
    }
    uint32_t target = m->kind == CALIB_KIND_IHRC ? c.q.target_ihrc : c.q.target_ilrc;
    calib_site_t s;
    calib_site_from_match(c.chip, c.base, m, target, c.q.mV, &s);
    calib_placeholder_words(c.chip->code_bits, &s, words);
    add_result(m->kind, m->k, target, MODE_TARGET, PDK_SITE_OK);
}

/* The word at the head of the window is plain code: FR-053 and serial number checks. */
static void plain(uint16_t w)
{
    op_result_t *r = op_current();
    uint16_t regs[3] = {c.chip->reg_ihrcr, c.chip->reg_ilrcr, c.chip->reg_bgtr};
    if (c.prev_mov && c.mov_a)
        for (unsigned i = 0; i < 3; i++)
            if (regs[i] != CHIP_REG_UNKNOWN && w == calib_mov_io_a(c.chip->code_bits, (uint8_t)regs[i]) &&
                !(r->warnings & PDK_WARN_CALIB_UNRECOGNIZED)) {
                r->warnings |= PDK_WARN_CALIB_UNRECOGNIZED;   /* a trim value the copies will share */
                r->fail_addr = c.prev_addr;
            }
    c.prev_mov = c.mov_a && (w & 0xFF00u) == c.mov_a;
    c.prev_addr = c.base;
    if (c.ret_op && (w & 0xFF00u) == c.ret_op) {
        c.ret_run++;
    } else {
        c.serial_seen |= c.ret_run == SERIAL_SITE_LEN;   /* exactly 8 RET #imm: a serial number */
        c.ret_run = 0;
    }
}

static void step(void)
{
    uint16_t words[CALIB_BG_LEN];
    calib_match_t m;
    unsigned len = 1;
    if (calib_match(c.chip, c.win, c.n, c.base, &m)) {
        len = m.len;
        memcpy(words, c.win, 2u * len);
        if (m.form == CALIB_FORM_FOOTPRINT)
            footprint(&m, words);
        else
            master_loop(&m, words);
        c.prev_mov = false;
        c.serial_seen |= c.ret_run == SERIAL_SITE_LEN;
        c.ret_run = 0;
    } else {
        words[0] = c.win[0];
        plain(c.win[0]);
    }
    for (unsigned i = 0; i < len; i++)
        out_word((uint16_t)(c.base + i), words[i]);
    memmove(c.win, c.win + len, 2u * (c.n - len));
    c.n = (uint8_t)(c.n - len);
    c.base = (uint16_t)(c.base + len);
}

static void copy_sink(void *ctx, uint16_t addr, uint16_t word)
{
    (void)ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / c.chip->code_words));
    uint8_t b[2] = {(uint8_t)word, (uint8_t)(word >> 8)};
    c.data_crc = crc32_update(c.data_crc, b, 2);
    image_crc_feed(&c.sums, word);
    c.last = word;
    c.win[c.n++] = word;
    if (c.n == WINDOW)
        step();
}

/* Mask, header (last: it names the master's checksum), trailer CRC. Returns the package size. */
static uint32_t finish_package(uint32_t norm)
{
    uint16_t words = c.chip->code_words;
    out_flush();
    uint8_t mask[64];
    uint32_t mask_bytes = 2u * ((words + 15u) / 16u);
    for (uint32_t off = 0; off < mask_bytes; off += sizeof mask) {
        uint32_t n = mask_bytes - off < sizeof mask ? mask_bytes - off : (uint32_t)sizeof mask;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t bits = 0;
            for (unsigned b = 0; b < 8; b++) {
                uint32_t a = 8u * (off + i) + b;
                if (a < words && !chip_addr_reserved(c.chip, (uint16_t)a))
                    bits |= (uint8_t)(1u << b);
            }
            mask[i] = bits;
        }
        if (!slots_write(c.slot, c.out_off, mask, n))
            c.write_failed = true;
        c.out_off += n;
    }
    uint32_t size = pkg_expected_size(words);
    pkg_header_t h;
    memset(&h, 0, sizeof h);
    h.package_size = size;
    h.chip_id12 = c.chip->id12;
    h.image_words = words;
    h.flags = PKG_F_CREATED_BY_COPY;
    snprintf(h.chip_name, sizeof h.chip_name, "%s", c.chip->name);
    if (c.q.name[0])
        snprintf(h.name, sizeof h.name, "%s", c.q.name);
    else
        snprintf(h.name, sizeof h.name, "copy %s %08lX", c.chip->name, (unsigned long)norm);
    uint8_t raw[PKG_HEADER_SIZE];
    pkg_header_build(&h, raw);
    if (!slots_write(c.slot, 0, raw, sizeof raw) || !slots_flush(c.slot))
        c.write_failed = true;
    uint8_t trailer[4];
    put32(trailer, slots_crc(c.slot, size - 4u));
    if (!slots_write(c.slot, size - 4u, trailer, 4) || !slots_flush(c.slot))
        c.write_failed = true;
    return size;
}

uint16_t op_copy_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_READ, PDK_STAGE_STORE};
    op_plan(stages, sizeof stages);
    op_result_t *r = op_current();
    op_read_invalidate();
    if (package_store_flags() & PDK_FLAG_MASTER_RESTORE_PENDING)
        return PDK_MASTER_RESTORE_PENDING;   /* the dump of a master must not be lost */
    uint16_t st = parse_request();
    if (st != PDK_OK)
        return st;

    const chip_info_t *chip;
    if ((st = op_read_identify(&chip)) != PDK_OK)
        return st;
    if (!chip_can_write(chip))
        return PDK_OP_UNSUPPORTED;   /* a package for it could never be written */
    if (c.q.mode_ihrc == MODE_MEASURE || c.q.mode_ilrc == MODE_MEASURE)
        return chip_is_flash(chip) ? PDK_OP_UNSUPPORTED : PDK_MASTER_NOT_REWRITABLE;   /* measuring: part 7c */

    op_stage(PDK_STAGE_READ);
    copy_req_t q = c.q;
    memset(&c, 0, sizeof c);
    c.q = q;
    c.chip = chip;
    c.mask = chip_word_mask(chip);
    c.ret_op = serial_ret_opcode(chip->code_bits);
    c.mov_a = calib_mov_a_imm(chip->code_bits);
    c.slot = package_store_upload_slot();
    slots_set_upload(c.slot);
    c.out_off = PKG_HEADER_SIZE;
    c.data_crc = crc32_init();
    image_crc_init(&c.sums, chip);
    if ((st = chip_engine_read(chip, 0, chip->code_words, copy_sink, NULL, op_poll)) != PDK_OK)
        return st;
    while (c.n)
        step();
    plain(0);   /* ends a serial run at the end of memory */
    r->data_words = chip->code_words;
    r->data_crc32 = crc32_final(c.data_crc);
    image_crc_finish(&c.sums, &r->image_crc_raw, &r->image_crc_norm);
    if (pkg_fuse_is_secure(c.last))
        return PDK_CHIP_PROTECTED;   /* R10 */
    if (c.loops && !(c.q.flags & COPY_ALLOW_MASTER_LOOP)) {
        r->fail_addr = c.first_loop;
        return PDK_COPY_MASTER_UNCALIBRATED;   /* the master is not calibrated: nothing stored */
    }
    if (c.loop_refused)
        return PDK_BAD_PAYLOAD;
    if (c.serial_seen)
        r->warnings |= PDK_WARN_SERIAL_DUPLICATED;

    op_stage(PDK_STAGE_STORE);
    uint32_t size = finish_package(r->image_crc_norm);
    if (c.write_failed)
        return PDK_STORE_WRITE_FAILED;
    /* the same checks as PKG_COMMIT, then it becomes the active package */
    uint8_t raw[PKG_HEADER_SIZE];
    slots_read(c.slot, 0, raw, sizeof raw);
    op_pkg_t *p = &op_pkg;
    if ((st = pkg_header_parse(raw, false, slots_size(), &p->hdr, &p->chip)) != PDK_OK)
        return st;
    pkg_slot_reader_init(&p->reader, c.slot, p->hdr.image_words);
    uint16_t bad;
    if ((st = pkg_validate(&p->hdr, p->chip, pkg_slot_word, &p->reader, &p->plan, &bad)) != PDK_OK) {
        r->fail_addr = bad;
        return st;
    }
    uint8_t trailer[4];
    slots_read(c.slot, size - 4u, trailer, 4);
    if (!package_store_commit(c.slot, size, le32(trailer)))
        return PDK_STORE_WRITE_FAILED;
    pkg_send_state_event();
    return PDK_OK;
}
