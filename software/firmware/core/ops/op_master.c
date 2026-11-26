/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The master rewritten from the active package, which holds its dump (research R9): with one footprint
 * turned into a measurement loop for MEASURE_MASTER, or as it was for the restore (COPY's RESTORE stage
 * and the RESTORE_MASTER operation, T114). The restore verifies the dump in the chip before
 * MASTER_RESTORE_PENDING is cleared, so a power loss at any point leaves either an intact master or the
 * flag set with the dump still stored. */
#include "op_master.h"

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "op_manager.h"
#include "op_pkg.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"

static op_pkg_t *const pk = &op_pkg;

static uint16_t word_at(uint16_t addr, bool fuse, const uint16_t *loop, uint16_t loop_addr, uint8_t loop_len)
{
    if (loop && addr >= loop_addr && addr < loop_addr + loop_len)
        return (uint16_t)(loop[addr - loop_addr] & pk->mask);
    eff_kind_t k = eff_classify(&pk->eff, addr).kind;
    if (k == EFF_RESERVED || k == EFF_UNWRITTEN || (k == EFF_FUSE && !fuse))
        return pk->mask;
    return eff_word(&pk->eff, addr);
}

/* Writes [first, last] in whole blocks; blocks that stay blank are skipped. */
static uint16_t write_range(uint16_t first, uint16_t last, bool fuse, const uint16_t *loop, uint16_t loop_addr,
                            uint8_t loop_len, chip_poll_fn poll)
{
    uint8_t bs = pk->chip->write_block ? pk->chip->write_block : 1u;
    uint16_t words[16];
    for (uint32_t a = (uint32_t)(first - first % bs); a <= last; a += bs) {
        bool any = false;
        for (unsigned i = 0; i < bs; i++) {
            uint32_t addr = a + i;
            words[i] = addr >= first && addr <= last && addr < pk->chip->code_words
                           ? word_at((uint16_t)addr, fuse, loop, loop_addr, loop_len)
                           : pk->mask;
            any |= words[i] != pk->mask;
        }
        if (any) {
            uint16_t st = chip_engine_write_block(pk->chip, (uint16_t)a, words, bs, poll);
            if (st != PDK_OK)
                return st;
        }
        op_percent((uint8_t)(a * 100u / pk->chip->code_words));
    }
    return PDK_OK;
}

uint16_t master_write_loop(const calib_site_t *site, uint8_t start_value, chip_poll_fn poll)
{
    uint16_t loop[CALIB_BG_LEN];
    uint8_t len = calib_loop_words(pk->chip->code_bits, site, (uint8_t)(start_value - 1u), loop);
    if (!len)
        return PDK_OP_UNSUPPORTED;
    uint16_t st = chip_engine_erase(pk->chip, poll);
    if (st != PDK_OK)
        return st;
    return write_range(0, (uint16_t)(pk->chip->code_words - 1u), false, loop, site->addr, len, poll);
}

typedef struct {
    bool fuse;
    uint16_t status;
} verify_t;

static void verify_sink(void *ctx, uint16_t addr, uint16_t word)
{
    verify_t *v = ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / pk->chip->code_words));
    if (!op_pkg_counted(pk->chip, addr) || (!v->fuse && addr == chip_fuse_addr(pk->chip)))
        return;
    uint16_t t = word_at(addr, true, NULL, 0, 0);
    if (word != t && v->status == PDK_OK) {
        op_result_t *r = op_current();
        r->fail_addr = addr;
        r->expected = t;
        r->actual = word;
        v->status = PDK_VERIFY_FAILED;
    }
}

static uint16_t verify(bool fuse, chip_poll_fn poll)
{
    verify_t v = {.fuse = fuse, .status = PDK_OK};
    uint16_t st = chip_engine_read(pk->chip, 0, pk->chip->code_words, verify_sink, &v, poll);
    return st != PDK_OK ? st : v.status;
}

uint16_t master_restore(bool stages, chip_poll_fn poll)
{
    uint16_t st;
    uint16_t fuse_addr = chip_fuse_addr(pk->chip);
    if (stages)
        op_stage(PDK_STAGE_ERASE);
    if ((st = chip_engine_erase(pk->chip, poll)) != PDK_OK)
        return st;
    if (stages)
        op_stage(PDK_STAGE_WRITE);
    if ((st = write_range(0, (uint16_t)(pk->chip->code_words - 1u), false, NULL, 0, 0, poll)) != PDK_OK)
        return st;
    if (stages)
        op_stage(PDK_STAGE_VERIFY);
    if ((st = verify(false, poll)) != PDK_OK)
        return st;
    if (pk->plan.fuse_defined) {
        if (stages)
            op_stage(PDK_STAGE_FUSE);
        if ((st = write_range(fuse_addr, fuse_addr, true, NULL, 0, 0, poll)) != PDK_OK)
            return st;
        if ((st = verify(true, poll)) != PDK_OK)
            return st;
        op_current()->fuse_written = pk->plan.fuse;
    }
    return package_store_set_pending(false) ? PDK_OK : PDK_STORE_WRITE_FAILED;
}

uint16_t op_restore_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_ERASE, PDK_STAGE_WRITE, PDK_STAGE_VERIFY, PDK_STAGE_FUSE};
    op_plan(stages, sizeof stages);
    op_stage(PDK_STAGE_ID);
    if (!(package_store_flags() & PDK_FLAG_MASTER_RESTORE_PENDING))
        return PDK_NOT_ALLOWED_NOW;   /* nothing to restore */
    uint16_t st = op_pkg_load();
    if (st != PDK_OK)
        return st;
    op_result_t *r = op_current();
    uint16_t id = 0;
    uint8_t family = 0;
    if ((st = chip_engine_probe(&id, &family, op_poll)) != PDK_OK)
        return st;
    r->chip_id_found = id;
    r->family_found = family;
    if (id != pk->chip->id12)
        return PDK_CHIP_ID_MISMATCH;
    if ((st = master_restore(true, op_poll)) != PDK_OK)
        return st;
    r->warnings |= PDK_WARN_MASTER_MODIFIED;
    return PDK_OK;
}
