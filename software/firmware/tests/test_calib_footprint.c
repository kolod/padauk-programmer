/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Measurement loops, finished forms and footprints against tests/vectors/footprints (gen_footprints.py). */
#include "pdk_test.h"

#include "chips/calib_footprint.h"
#include "chips/chip_table.h"
#include "fake_hal.h"
#include "vectors_footprints.h"

void pdk_test_setup(void) { fake_reset(); }

static const chip_info_t *chip_named(const char *name)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), name))
            return chip_at(i);
    return NULL;
}

typedef struct {
    const uint16_t *w;
    uint16_t count;
} array_t;

static uint16_t array_get(void *ctx, uint16_t addr, bool *defined)
{
    const array_t *a = ctx;
    *defined = addr < a->count;
    return *defined ? a->w[addr] : 0xFFFF;
}

static void site_of(const fp_loop_vector_t *v, calib_site_t *s)
{
    s->addr = v->pos;
    s->kind = v->kind;
    s->len = v->kind == CALIB_KIND_BG ? CALIB_BG_LEN : CALIB_RC_LEN;
    s->target_hz = v->target_hz;
    s->mV = v->mv;
    s->reg0 = v->regs[0];
    s->reg1 = v->regs[1];
    s->reg2 = v->regs[2];
}

TEST(loops_and_finished_forms_match_free_pdk)
{
    for (size_t i = 0; i < sizeof fp_loops / sizeof fp_loops[0]; i++) {
        const fp_loop_vector_t *v = &fp_loops[i];
        calib_site_t s;
        site_of(v, &s);
        uint16_t loop[14], fin[14];
        ASSERT_EQ(calib_loop_words(v->bits, &s, CALIB_LOOP_IMM, loop), s.len);
        ASSERT_MEM_EQ(loop, v->loop, 2u * s.len);
        ASSERT_EQ(calib_final_words(v->bits, &s, v->k, fin), s.len);
        ASSERT_MEM_EQ(fin, v->final, 2u * s.len);
        for (unsigned j = 0; j < s.len; j++)   /* loop → finished: 1 → 0 only */
            ASSERT_EQ(fin[j] & loop[j], fin[j]);
    }
}

TEST(placeholder_round_trip)
{
    for (size_t i = 0; i < sizeof fp_loops / sizeof fp_loops[0]; i++) {
        const fp_loop_vector_t *v = &fp_loops[i];
        calib_site_t s, found[CALIB_MAX_SITES];
        site_of(v, &s);
        uint16_t ph[14];
        ASSERT_EQ(calib_placeholder_words(v->bits, &s, ph), s.len);
        ASSERT_MEM_EQ(ph, v->placeholder, 2u * s.len);
        array_t a = {ph, s.len};
        uint8_t n = 0;
        uint16_t bad;
        ASSERT_EQ(calib_find_sites(v->bits, array_get, &a, s.len, found, &n, &bad), 0);
        ASSERT_EQ(n, 1);
        ASSERT_EQ(found[0].kind, s.kind);
        ASSERT_EQ(found[0].target_hz, s.target_hz);
        ASSERT_EQ(found[0].mV, s.mV);
        ASSERT_EQ(found[0].reg0, s.reg0);
        ASSERT_EQ(found[0].reg1, s.reg1);
        ASSERT_EQ(found[0].reg2, s.reg2);
    }
}

TEST(loops_and_finished_forms_are_recognised_where_they_were_written)
{
    for (size_t i = 0; i < sizeof fp_loops / sizeof fp_loops[0]; i++) {
        const fp_loop_vector_t *v = &fp_loops[i];
        const chip_info_t *c = chip_named(v->chip);
        ASSERT_TRUE(c != NULL);
        calib_site_t s;
        site_of(v, &s);
        calib_match_t m;
        ASSERT_TRUE(calib_match(c, v->loop, s.len, v->pos, &m));
        ASSERT_EQ(m.form, CALIB_FORM_LOOP);
        ASSERT_EQ(m.kind, v->kind);
        ASSERT_EQ(m.k, CALIB_LOOP_IMM);
        ASSERT_TRUE(!calib_match(c, v->loop, s.len, (uint16_t)(v->pos + 1), &m));   /* GOTO elsewhere */
        ASSERT_TRUE(calib_match(c, v->final, s.len, v->pos, &m));
        ASSERT_EQ(m.form, CALIB_FORM_FOOTPRINT);
        ASSERT_EQ(m.kind, v->kind);
        ASSERT_EQ(m.k, v->k);
        ASSERT_TRUE(!calib_match(c, v->final, (unsigned)(s.len - 1), v->pos, &m));   /* cut short */
    }
}

TEST(dumps_give_exactly_the_expected_footprints_and_loops)
{
    for (size_t i = 0; i < sizeof fp_dumps / sizeof fp_dumps[0]; i++) {
        const fp_dump_vector_t *d = &fp_dumps[i];
        const chip_info_t *c = chip_named(d->chip);
        ASSERT_TRUE(c != NULL);
        array_t a = {d->words, d->count};
        uint16_t addrs[16];
        calib_match_t m[16];
        uint8_t n = calib_scan(c, array_get, &a, d->count, addrs, m, 16);
        ASSERT_EQ(n, d->n_matches);   /* foreign register, bare trim write, misplaced GOTO: no match */
        for (unsigned j = 0; j < n; j++) {
            ASSERT_EQ(addrs[j], d->matches[j].addr);
            ASSERT_EQ(m[j].form, d->matches[j].form);
            ASSERT_EQ(m[j].kind, d->matches[j].kind);
            ASSERT_EQ(m[j].k, d->matches[j].k);
            ASSERT_EQ(m[j].len, d->matches[j].len);
        }
    }
}

TEST(placeholders_are_restored_bit_for_bit)
{
    for (size_t i = 0; i < sizeof fp_dumps / sizeof fp_dumps[0]; i++) {
        const fp_dump_vector_t *d = &fp_dumps[i];
        const chip_info_t *c = chip_named(d->chip);
        for (unsigned j = 0; j < d->n_restore; j++) {
            const fp_restore_t *r = &d->restore[j];
            calib_match_t m;
            ASSERT_TRUE(calib_match(c, d->words + r->addr, (unsigned)(d->count - r->addr), r->addr, &m));
            calib_site_t s;
            calib_site_from_match(c, r->addr, &m, r->target_hz, r->mv, &s);
            uint16_t ph[14];
            uint8_t len = calib_placeholder_words(c->code_bits, &s, ph);
            ASSERT_EQ(len, m.len);
            ASSERT_MEM_EQ(ph, r->words, 2u * len);
        }
    }
}

TEST(chips_without_known_registers_never_match)
{
    const chip_info_t *c = chip_named("PMS131");   /* trim register addresses unknown */
    const fp_loop_vector_t *v = &fp_loops[3];      /* 14-bit IHRC */
    calib_match_t m;
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(!calib_match(c, v->final, CALIB_RC_LEN, v->pos, &m));
    ASSERT_TRUE(!calib_match(c, v->loop, CALIB_RC_LEN, v->pos, &m));
}
