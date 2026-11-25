/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Effective image of a package: unset words, SECURE_FILL, fuse, serial number, calibration sites. */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "fake_hal.h"
#include "package/effective_image.h"
#include "proto/proto_defs.h"
#include "vectors_footprints.h"

#define MAX_WORDS 0xC00u

static struct {
    uint16_t w[MAX_WORDS];
    uint8_t def[MAX_WORDS];
    pkg_header_t h;
    pkg_plan_t plan;
    eff_image_t e;
    const chip_info_t *chip;
} t;

void pdk_test_setup(void)
{
    fake_reset();
    memset(&t, 0, sizeof t);
}

static uint16_t get(void *ctx, uint16_t addr, bool *defined)
{
    (void)ctx;
    *defined = addr < t.h.image_words && t.def[addr];
    return *defined ? t.w[addr] : 0xFFFF;
}

static void use_chip(const char *name)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), name))
            t.chip = chip_at(i);
    t.h.chip_id12 = t.chip->id12;
}

static void put(uint16_t addr, uint16_t w)
{
    t.w[addr] = w;
    t.def[addr] = 1;
    if (addr >= t.h.image_words)
        t.h.image_words = (uint16_t)(addr + 1u);
}

static uint16_t build(void)
{
    uint16_t bad;
    uint16_t st = pkg_validate(&t.h, t.chip, get, NULL, &t.plan, &bad);
    if (st == PDK_OK)
        eff_init(&t.e, t.chip, &t.h, &t.plan, get, NULL);
    return st;
}

TEST(unset_words_are_not_written)
{
    use_chip("PFS154");
    put(0, 0x1234);
    put(3, 0xFFFF);   /* bits above the 14-bit core are ignored */
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(eff_classify(&t.e, 0).kind, EFF_PLAIN);
    ASSERT_EQ(eff_word(&t.e, 0), 0x1234);
    ASSERT_EQ(eff_word(&t.e, 3), 0x3FFF);
    ASSERT_EQ(eff_classify(&t.e, 1).kind, EFF_UNWRITTEN);
    ASSERT_EQ(eff_word(&t.e, 1), 0x3FFF);
    ASSERT_EQ(eff_classify(&t.e, 0x100).kind, EFF_UNWRITTEN);   /* beyond image_words */
}

TEST(secure_fill_reaches_the_reserved_area_only)
{
    use_chip("PFS154");   /* reserved 0x7E0 … 0x7F0 */
    put(0, 0x1234);
    t.h.flags = PKG_F_SECURE_FILL;
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(eff_classify(&t.e, 1).kind, EFF_PLAIN);
    ASSERT_EQ(eff_word(&t.e, 1), 0x0000);
    ASSERT_EQ(eff_word(&t.e, 0x7DF), 0x0000);
    ASSERT_EQ(eff_classify(&t.e, 0x7E0).kind, EFF_RESERVED);
    ASSERT_EQ(eff_classify(&t.e, 0x7F0).kind, EFF_RESERVED);
    ASSERT_EQ(eff_classify(&t.e, 0x7F1).kind, EFF_UNWRITTEN);
    ASSERT_EQ(eff_word(&t.e, 0x7F1), 0x3FFF);
}

TEST(fuse_is_kept_apart)
{
    use_chip("PFS154");
    put(0, 0x1234);
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(eff_classify(&t.e, 0x7FF).kind, EFF_FUSE);
    ASSERT_EQ(eff_word(&t.e, 0x7FF), 0x3FFF);   /* no fuse: stays blank */

    put(0x7FF, 0x3FFD);
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(eff_classify(&t.e, 0x7FF).kind, EFF_FUSE);
    ASSERT_EQ(eff_word(&t.e, 0x7FF), 0x3FFD);

    memset(&t, 0, sizeof t);
    use_chip("PFS154");
    put(0, 0x1234);
    t.h.flags = PKG_F_FUSE_PRESENT;
    t.h.fuse = 0x3FFB;
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(eff_word(&t.e, 0x7FF), 0x3FFB);
}

TEST(serial_number_bytes_go_low_byte_first)
{
    use_chip("PFS154");
    static const char marker[] = "FPSERIAL";
    for (unsigned i = 0; i < 8; i++)
        put((uint16_t)(0x10 + i), (uint16_t)(0x0200u | (uint8_t)marker[i]));
    ASSERT_EQ(build(), PDK_OK);
    ASSERT_EQ(t.plan.serial_count, 1);
    static const uint8_t serial[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    eff_set_serial(&t.e, serial);
    for (unsigned i = 0; i < 8; i++) {
        eff_class_t c = eff_classify(&t.e, (uint16_t)(0x10 + i));
        ASSERT_EQ(c.kind, EFF_SERIAL);
        ASSERT_EQ(c.index, 0);
        ASSERT_EQ(c.offset, i);
        ASSERT_EQ(eff_word(&t.e, (uint16_t)(0x10 + i)), 0x0200u | serial[i]);
    }
}

TEST(placeholder_becomes_loop_then_finished_form)
{
    for (size_t i = 0; i < sizeof fp_loops / sizeof fp_loops[0]; i++) {
        const fp_loop_vector_t *v = &fp_loops[i];
        memset(&t, 0, sizeof t);
        use_chip(v->chip);
        uint8_t len = v->kind == CALIB_KIND_BG ? CALIB_BG_LEN : CALIB_RC_LEN;
        for (unsigned j = 0; j < len; j++)
            put((uint16_t)(v->pos + j), v->placeholder[j]);
        ASSERT_EQ(build(), PDK_OK);
        ASSERT_EQ(t.plan.calib_count, 1);
        uint16_t loop[14];
        for (unsigned j = 0; j < len; j++) {
            eff_class_t c = eff_classify(&t.e, (uint16_t)(v->pos + j));
            ASSERT_EQ(c.kind, EFF_CALIB);
            ASSERT_EQ(c.offset, j);
            loop[j] = eff_word(&t.e, (uint16_t)(v->pos + j));
            ASSERT_EQ(loop[j], v->loop[j]);   /* fpdkiccalib.c opcodes */
        }
        eff_set_final(&t.e, 0, v->k);
        for (unsigned j = 0; j < len; j++) {
            uint16_t w = eff_word(&t.e, (uint16_t)(v->pos + j));
            ASSERT_EQ(w, v->final[j]);
            ASSERT_EQ(w & loop[j], w);   /* loop → finished: 1 → 0 only */
        }
    }
}

TEST(loop_can_start_from_a_given_value)
{
    const fp_loop_vector_t *v = &fp_loops[3];   /* 14-bit IHRC */
    use_chip(v->chip);
    for (unsigned j = 0; j < CALIB_RC_LEN; j++)
        put((uint16_t)(v->pos + j), v->placeholder[j]);
    ASSERT_EQ(build(), PDK_OK);
    eff_set_loop(&t.e, 0, 0x41);
    ASSERT_EQ(eff_word(&t.e, (uint16_t)(v->pos + 2)), (v->loop[2] & 0xFF00u) | 0x41u);
    ASSERT_EQ(eff_word(&t.e, (uint16_t)(v->pos + 9)), v->loop[9]);
}
