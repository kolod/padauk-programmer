/* SPDX-License-Identifier: GPL-3.0-or-later */
/* image_crc_raw / image_crc_norm: vectors, excluded words, normalisation, package vs programmed chip. */
#include "pdk_test.h"

#include <stdlib.h>

#include "chips/calib_footprint.h"
#include "fake_hal.h"
#include "package/image_crc.h"
#include "proto/proto_defs.h"
#include "vectors_footprints.h"

#define MAX_WORDS 0xC00u

static uint16_t img[MAX_WORDS];

void pdk_test_setup(void) { fake_reset(); }

static const chip_info_t *chip_named(const char *name)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), name))
            return chip_at(i);
    return NULL;
}

static void sums(const chip_info_t *c, const uint16_t *w, uint32_t *raw, uint32_t *norm)
{
    image_crc_t s;
    image_crc_init(&s, c);
    for (uint16_t a = 0; a < c->code_words; a++)
        image_crc_feed(&s, w[a]);
    image_crc_finish(&s, raw, norm);
}

static const fp_dump_vector_t *dump_of(const char *chip)
{
    for (size_t i = 0; i < sizeof fp_dumps / sizeof fp_dumps[0]; i++)
        if (!strcmp(fp_dumps[i].chip, chip))
            return &fp_dumps[i];
    return NULL;
}

TEST(vectors)
{
    for (size_t i = 0; i < sizeof fp_dumps / sizeof fp_dumps[0]; i++) {
        const fp_dump_vector_t *d = &fp_dumps[i];
        uint32_t raw, norm;
        sums(chip_named(d->chip), d->words, &raw, &norm);
        ASSERT_EQ(raw, d->image_crc_raw);
        ASSERT_EQ(norm, d->image_crc_norm);
    }
}

TEST(reserved_words_first_word_and_high_bits_do_not_count)
{
    const chip_info_t *c = chip_named("PMS154B");   /* address 0 excluded, reserved 0x7E8 … */
    uint32_t raw0, norm0, raw, norm;
    for (unsigned a = 0; a < c->code_words; a++)
        img[a] = (uint16_t)(a * 7u & 0x3FFFu);
    sums(c, img, &raw0, &norm0);
    img[0] ^= 0x155;
    img[0x7E8] ^= 0x2AA;
    img[0x100] |= 0xC000;   /* above the 14-bit core */
    sums(c, img, &raw, &norm);
    ASSERT_EQ(raw, raw0);
    ASSERT_EQ(norm, norm0);
    img[0x7FF] ^= 1;        /* the fuse counts */
    sums(c, img, &raw, &norm);
    ASSERT_TRUE(raw != raw0);
}

TEST(same_firmware_different_k_and_serial_gives_the_same_norm)
{
    const fp_dump_vector_t *d = dump_of("PFS154");
    const chip_info_t *c = chip_named(d->chip);
    memcpy(img, d->words, 2u * d->count);
    img[d->matches[0].addr + 2] ^= 0x0F;   /* IHRC footprint: other k */
    img[d->matches[2].addr + 7] ^= 0xF0;   /* BG footprint: other k */
    for (unsigned i = 0; i < 8; i++)       /* other serial number */
        img[d->serial_addr + i] ^= (uint16_t)(0x11 * (i + 1) & 0xFF);
    uint32_t raw, norm;
    sums(c, img, &raw, &norm);
    ASSERT_EQ(norm, d->image_crc_norm);
    ASSERT_TRUE(raw != d->image_crc_raw);
}

TEST(loops_ret_tables_and_code_are_not_normalised)
{
    const fp_dump_vector_t *d = dump_of("PFS173");
    const chip_info_t *c = chip_named(d->chip);
    uint32_t raw, norm;
    memcpy(img, d->words, 2u * d->count);
    img[d->matches[3].addr + 2] ^= 0x01;   /* measurement loop immediate */
    sums(c, img, &raw, &norm);
    ASSERT_TRUE(norm != d->image_crc_norm);
    memcpy(img, d->words, 2u * d->count);
    img[0x120 + 4] ^= 0x01;                /* inside the 9-entry RET table */
    sums(c, img, &raw, &norm);
    ASSERT_TRUE(norm != d->image_crc_norm);
    memcpy(img, d->words, 2u * d->count);
    img[0x300] ^= 0x0100;
    sums(c, img, &raw, &norm);
    ASSERT_TRUE(norm != d->image_crc_norm);
}

TEST(serial_run_at_the_very_end_is_normalised)
{
    const chip_info_t *c = chip_named("PFS154");
    uint32_t raw0, norm0, raw, norm;
    memset(img, 0, sizeof img);
    for (unsigned i = 0; i < 8; i++)
        img[c->code_words - 8 + i] = (uint16_t)(0x0200u | i);   /* the fuse word ends the run */
    sums(c, img, &raw0, &norm0);
    img[c->code_words - 3] ^= 0x40;
    sums(c, img, &raw, &norm);
    ASSERT_EQ(norm, norm0);
    ASSERT_TRUE(raw != raw0);
}

/* ---- the sum of a package's effective image equals the sum of the chip it programs ---- */

static struct {
    uint16_t w[MAX_WORDS];
    uint8_t def[MAX_WORDS];
    pkg_header_t h;
    pkg_plan_t plan;
} pk;

static uint16_t pk_get(void *ctx, uint16_t addr, bool *defined)
{
    (void)ctx;
    *defined = addr < pk.h.image_words && pk.def[addr];
    return *defined ? pk.w[addr] : 0xFFFF;
}

static void pk_put(uint16_t addr, uint16_t w)
{
    pk.w[addr] = w;
    pk.def[addr] = 1;
    if (addr >= pk.h.image_words)
        pk.h.image_words = (uint16_t)(addr + 1u);
}

TEST(package_sum_equals_programmed_chip)
{
    const fp_loop_vector_t *rc = &fp_loops[3], *bg = &fp_loops[5];   /* PFS154 IHRC, BG */
    const chip_info_t *c = chip_named("PFS154");
    memset(&pk, 0, sizeof pk);
    pk.h.chip_id12 = c->id12;
    srand(7);
    for (uint16_t a = 0; a < 0x400; a++)
        pk_put(a, (uint16_t)(0x3000u | (rand() & 0x0FFF)));   /* GOTO-ish code: no placeholders */
    for (unsigned j = 0; j < CALIB_RC_LEN; j++)
        pk_put((uint16_t)(rc->pos + j), rc->placeholder[j]);
    for (unsigned j = 0; j < CALIB_BG_LEN; j++)
        pk_put((uint16_t)(bg->pos + j), bg->placeholder[j]);
    static const char marker[] = "FPSERIAL";
    for (unsigned i = 0; i < 8; i++)
        pk_put((uint16_t)(0x200 + i), (uint16_t)(0x0200u | (uint8_t)marker[i]));
    pk_put(0x7FF, 0x3FFD);
    uint16_t bad;
    ASSERT_EQ(pkg_validate(&pk.h, c, pk_get, NULL, &pk.plan, &bad), PDK_OK);
    ASSERT_EQ(pk.plan.calib_count, 2);

    /* what the programmer knows before writing: finished forms with k = 0, serial 0 */
    eff_image_t e;
    eff_init(&e, c, &pk.h, &pk.plan, pk_get, NULL);
    eff_set_final(&e, 0, 0);
    eff_set_final(&e, 1, 0);
    uint32_t pkg_raw, pkg_norm;
    image_crc_of_eff(&e, &pkg_raw, &pkg_norm);

    /* the chip after PROGRAM: real k, a random serial, blank where nothing was written */
    static const uint8_t serial[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
    eff_set_final(&e, 0, 0x5A);
    eff_set_final(&e, 1, 0x93);
    eff_set_serial(&e, serial);
    for (uint16_t a = 0; a < c->code_words; a++)
        img[a] = eff_word(&e, a);
    uint32_t chip_raw, chip_norm;
    sums(c, img, &chip_raw, &chip_norm);
    ASSERT_EQ(chip_norm, pkg_norm);
    ASSERT_TRUE(chip_raw != pkg_raw);
    uint32_t eff_raw, eff_norm;   /* and the effective image with the real values gives the chip's raw sum */
    image_crc_of_eff(&e, &eff_raw, &eff_norm);
    ASSERT_EQ(eff_raw, chip_raw);
    ASSERT_EQ(eff_norm, chip_norm);
}
