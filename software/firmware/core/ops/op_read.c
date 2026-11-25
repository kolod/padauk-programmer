/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "op_read.h"

#include <string.h>

#include "chip_engine.h"
#include "op_manager.h"
#include "package/header.h"
#include "package/image_crc.h"
#include "pdk_config.h"
#include "proto/crc32.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"
#include "storage/slots.h"

static struct {
    bool valid;
    uint8_t slot;
    uint32_t words;
} rr;

void op_read_invalidate(void) { rr.valid = false; }

typedef struct {
    const chip_info_t *chip;
    uint8_t slot;
    uint32_t offset, crc;
    bool write_failed;
    uint16_t last;             /* the fuse word, read last */
    uint8_t buf[64];
    uint8_t n;
    image_crc_t sums;
} read_ctx_t;

static void flush_buf(read_ctx_t *c)
{
    if (c->n && !slots_write(c->slot, c->offset, c->buf, c->n))
        c->write_failed = true;
    c->offset += c->n;
    c->n = 0;
}

static void read_sink(void *ctx, uint16_t addr, uint16_t word)
{
    read_ctx_t *c = ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / c->chip->code_words));
    put16(c->buf + c->n, word);
    c->crc = crc32_update(c->crc, c->buf + c->n, 2);
    c->n += 2;
    if (c->n == sizeof c->buf)
        flush_buf(c);
    image_crc_feed(&c->sums, word);
    c->last = word;
}

uint16_t op_read_identify(const chip_info_t **chip)
{
    op_result_t *r = op_current();
    uint16_t want = op_request()->chip_id12;
    op_stage(PDK_STAGE_ID);
    uint16_t id = 0;
    uint8_t family = 0;
    uint16_t st = chip_engine_probe(&id, &family, op_poll);
    if (st != PDK_OK)
        return st;
    r->chip_id_found = id;
    r->family_found = family;
    *chip = chip_by_id(want ? want : id);
    if (!*chip)
        return want ? PDK_PKG_CHIP_UNSUPPORTED : PDK_CHIP_ID_MISMATCH;   /* a chip the table does not know */
    if (id != (*chip)->id12)
        return PDK_CHIP_ID_MISMATCH;
    return (chip_ops(*chip) & PDK_CHIP_OP_READ) ? PDK_OK : PDK_OP_UNSUPPORTED;
}

uint16_t op_read_chip(const chip_info_t *chip, uint8_t *slot)
{
    op_result_t *r = op_current();
    op_read_invalidate();
    op_stage(PDK_STAGE_READ);
    static read_ctx_t c;
    memset(&c, 0, sizeof c);
    c.chip = chip;
    c.slot = package_store_upload_slot();   /* the inactive slot; writing it keeps it from being erased */
    slots_set_upload(c.slot);               /* a new write session: pages written before get erased again */
    c.crc = crc32_init();
    image_crc_init(&c.sums, chip);
    uint16_t st = chip_engine_read(chip, 0, chip->code_words, read_sink, &c, op_poll);
    if (st != PDK_OK)
        return st;
    flush_buf(&c);
    if (c.write_failed || !slots_flush(c.slot))
        return PDK_STORE_WRITE_FAILED;
    if (pkg_fuse_is_secure(c.last))   /* R10: read protection on — the data mean nothing */
        return PDK_CHIP_PROTECTED;
    r->data_words = chip->code_words;
    r->data_crc32 = crc32_final(c.crc);
    image_crc_finish(&c.sums, &r->image_crc_raw, &r->image_crc_norm);
    rr.slot = c.slot;
    rr.words = chip->code_words;
    rr.valid = true;
    *slot = c.slot;
    return PDK_OK;
}

uint16_t op_read_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_READ};
    op_plan(stages, sizeof stages);
    const chip_info_t *chip;
    op_read_invalidate();
    uint16_t st = op_read_identify(&chip);
    if (st != PDK_OK)
        return st;
    uint8_t slot;
    return op_read_chip(chip, &slot);
}

uint16_t op_cmd_result_read(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    if (len != 6)
        return PDK_BAD_PAYLOAD;
    if (!rr.valid)
        return PDK_NOT_ALLOWED_NOW;
    uint32_t offset = le32(req);
    uint16_t n = le16(req + 4);
    if (n > PDK_MAX_PAYLOAD || offset > 2u * rr.words || n > 2u * rr.words - offset)
        return PDK_BAD_PAYLOAD;
    slots_read(rr.slot, offset, rsp, n);
    *rsp_len = n;
    return PDK_OK;
}
