/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PATCH (contracts/protocol.md §4): ID → READ (whole memory: checksums and the current words) → checksum
 * and 1→0 checks, both complete before anything is written → WRITE (changed words only, whole blocks,
 * ones elsewhere) → VERIFY (the raw checksum must be the original one with the values applied). */
#ifdef PDK_FEATURE_PATCH

#include <string.h>

#include "chip_engine.h"
#include "op_manager.h"
#include "op_pkg.h"
#include "package/header.h"
#include "package/image_crc.h"
#include "proto/le.h"
#include "proto/proto_defs.h"

#define PATCH_MAX_WORDS 64u

enum { PATCH_CHECK_ONLY = 1u << 0, PATCH_ALLOW_SECURITY_FUSE = 1u << 1 };
enum { EXPECT_RAW = 0, EXPECT_NORMALIZED = 1, EXPECT_ACTIVE_PACKAGE = 2 };

static struct {
    const chip_info_t *chip;
    uint8_t count;
    uint16_t addr[PATCH_MAX_WORDS], value[PATCH_MAX_WORDS], current[PATCH_MAX_WORDS];
    image_crc_t now, after;   /* the content as read, and with the values applied */
    uint32_t after_raw;
} t;

static uint16_t mismatch(uint16_t status, uint16_t addr, uint16_t expected, uint16_t actual)
{
    op_result_t *r = op_current();
    r->fail_addr = addr;
    r->expected = expected;
    r->actual = actual;
    return status;
}

static int index_of(uint16_t addr)
{
    for (unsigned i = 0; i < t.count; i++)
        if (t.addr[i] == addr)
            return (int)i;
    return -1;
}

static void read_sink(void *ctx, uint16_t addr, uint16_t word)
{
    (void)ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / t.chip->code_words));
    int i = index_of(addr);
    if (i >= 0)
        t.current[i] = word;
    image_crc_feed(&t.now, word);
    image_crc_feed(&t.after, i >= 0 ? t.value[i] : word);
}

static void verify_sink(void *ctx, uint16_t addr, uint16_t word)
{
    (void)ctx;
    if (addr % 64u == 0)
        op_percent((uint8_t)((uint32_t)addr * 100u / t.chip->code_words));
    image_crc_feed(&t.now, word);
}

/* image_crc_norm of the active package as it is after PROGRAM: finished calibration, any serial. */
static uint16_t package_norm(uint32_t *norm)
{
    uint16_t st = op_pkg_load();
    if (st != PDK_OK)
        return st;
    if (op_pkg.chip != t.chip)
        return PDK_CHIP_ID_MISMATCH;
    for (uint8_t i = 0; i < op_pkg.plan.calib_count; i++)
        eff_set_final(&op_pkg.eff, i, 0);
    uint32_t raw;
    image_crc_of_eff(&op_pkg.eff, &raw, norm);
    return PDK_OK;
}

uint16_t op_patch_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_ID, PDK_STAGE_READ, PDK_STAGE_WRITE, PDK_STAGE_VERIFY};
    op_plan(stages, sizeof stages);
    op_result_t *r = op_current();
    const op_request_t *q = op_request();
    t.chip = chip_by_id(q->chip_id12);
    if (!t.chip || q->ext_len < 7)
        return PDK_BAD_PAYLOAD;
    uint8_t flags = q->ext[0], mode = q->ext[1], count = q->ext[6];
    uint32_t expected_crc = le32(q->ext + 2);
    bool check_only = flags & PATCH_CHECK_ONLY;
    if (mode > EXPECT_ACTIVE_PACKAGE || count > PATCH_MAX_WORDS || (!count && !check_only) ||
        q->ext_len != 7u + 4u * count)
        return PDK_BAD_PAYLOAD;
    uint16_t mask = chip_word_mask(t.chip);
    uint16_t fuse_addr = chip_fuse_addr(t.chip);
    t.count = count;
    for (unsigned i = 0; i < count; i++) {
        t.addr[i] = le16(q->ext + 7 + 4 * i);
        t.value[i] = (uint16_t)(le16(q->ext + 9 + 4 * i) & mask);
        if (t.addr[i] >= t.chip->code_words || index_of(t.addr[i]) != (int)i)
            return PDK_BAD_PAYLOAD;   /* out of the chip, or the same address twice */
        if (chip_addr_reserved(t.chip, t.addr[i]))
            return mismatch(PDK_PKG_RESERVED_AREA, t.addr[i], t.value[i], 0);
        if (t.addr[i] == fuse_addr && pkg_fuse_is_secure(t.value[i]) && !(flags & PATCH_ALLOW_SECURITY_FUSE))
            return mismatch(PDK_PKG_SECURITY_NOT_ALLOWED, t.addr[i], t.value[i], 0);
    }
    uint32_t want_norm = 0;
    if (mode == EXPECT_ACTIVE_PACKAGE) {
        uint16_t st = package_norm(&want_norm);
        if (st != PDK_OK)
            return st;
    }

    op_stage(PDK_STAGE_ID);
    uint16_t id = 0;
    uint8_t family = 0;
    uint16_t st = chip_engine_probe(&id, &family, op_poll);
    if (st != PDK_OK)
        return st;
    r->chip_id_found = id;
    r->family_found = family;
    if (id != t.chip->id12)
        return PDK_CHIP_ID_MISMATCH;

    op_stage(PDK_STAGE_READ);
    image_crc_init(&t.now, t.chip);
    image_crc_init(&t.after, t.chip);
    if ((st = chip_engine_read(t.chip, 0, t.chip->code_words, read_sink, NULL, op_poll)) != PDK_OK)
        return st;
    uint32_t after_norm;
    image_crc_finish(&t.now, &r->image_crc_raw, &r->image_crc_norm);
    image_crc_finish(&t.after, &t.after_raw, &after_norm);

    /* both checks before any write */
    bool compare = !(check_only && mode != EXPECT_ACTIVE_PACKAGE && expected_crc == 0);
    uint32_t have = mode == EXPECT_RAW ? r->image_crc_raw : r->image_crc_norm;
    uint32_t want = mode == EXPECT_ACTIVE_PACKAGE ? want_norm : expected_crc;
    if (compare && have != want)
        return PDK_PATCH_IMAGE_MISMATCH;
    for (unsigned i = 0; i < count; i++)
        if ((t.current[i] & t.value[i]) != t.value[i])
            return mismatch(PDK_PATCH_CONFLICT, t.addr[i], t.value[i], t.current[i]);
    if (check_only)
        return PDK_OK;

    op_stage(PDK_STAGE_WRITE);
    uint8_t bs = t.chip->write_block ? t.chip->write_block : 1u;
    uint16_t words[16];
    for (unsigned i = 0; i < count; i++) {
        if (t.current[i] == t.value[i])
            continue;   /* already there */
        uint16_t first = (uint16_t)(t.addr[i] - t.addr[i] % bs);
        bool done_before = false;   /* the block was written with an earlier word of the list */
        for (unsigned j = 0; j < i; j++)
            done_before |= t.current[j] != t.value[j] && t.addr[j] - t.addr[j] % bs == first;
        if (done_before)
            continue;
        for (unsigned k = 0; k < bs; k++) {
            int n = index_of((uint16_t)(first + k));
            words[k] = n >= 0 && t.current[n] != t.value[n] ? t.value[n] : mask;
        }
        if ((st = chip_engine_write_block(t.chip, first, words, bs, op_poll)) != PDK_OK)
            return st;
    }
    for (unsigned i = 0; i < count; i++)
        r->data_words += t.current[i] != t.value[i];

    op_stage(PDK_STAGE_VERIFY);
    image_crc_init(&t.now, t.chip);
    if ((st = chip_engine_read(t.chip, 0, t.chip->code_words, verify_sink, NULL, op_poll)) != PDK_OK)
        return st;
    image_crc_finish(&t.now, &r->image_crc_raw, &r->image_crc_norm);
    return r->image_crc_raw == t.after_raw ? PDK_OK : PDK_VERIFY_FAILED;
}

#endif
