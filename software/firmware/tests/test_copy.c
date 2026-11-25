/* SPDX-License-Identifier: GPL-3.0-or-later */
/* COPY makes a package from a master dump (T105): fuse from the last word, CREATED_BY_COPY, name, the
 * placeholders by mode for each kind, BandGap restore, SNAP_STANDARD encoding, the serial number copied
 * with SERIAL_DUPLICATED, a trim write outside a footprint with CALIB_UNRECOGNIZED, protected and
 * read-only chips. */
#include "pdk_test.h"

#include <stdio.h>

#include "chips/calib_footprint.h"
#include "fake_chip.h"
#include "fake_hal.h"
#include "ops/op_pkg.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "storage/package_store.h"
#include "test_copy_helpers.h"
#include "test_ops.h"

#define IHRC 0x40u
#define ILRC 0x50u
#define BG 0x60u
#define SERIAL 0x80u

static tops_image_t img;
static const chip_info_t *chip;
static uint16_t master[TOPS_MAX_WORDS];

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    pdk_core_poll();   /* the start-up SELF_TEST */
    tops_seq = 1;
    chip = tops_chip("PFS154");
    tops_image_init(&img);
    tops_put_code(&img, chip, 0, 0x40);
    tops_put_calib(&img, chip, IHRC, CALIB_KIND_IHRC, 8000000, 4000);
    tops_put_calib(&img, chip, ILRC, CALIB_KIND_ILRC, 62500, 4000);
    tops_put_calib(&img, chip, BG, CALIB_KIND_BG, 0, 3950);
    tops_put_serial(&img, chip, SERIAL);
    tops_put_code(&img, chip, 0x90, 0x20);
    tops_put(&img, 0x7FF, 0x3FFD);
}

/* The master: the image programmed with k = 0x5A (IHRC), 0x33 (ILRC), 0x77 (BG). */
static void make_master(void)
{
    fake_chip_insert(chip);
    fake_chip_calib(FAKE_CAL_OK, 0x5A);
    fake_chip_calib(FAKE_CAL_OK, 0x33);
    fake_chip_calib(FAKE_CAL_OK, 0x77);
    ASSERT_EQ(tops_upload(chip, &img), TP_STAGE_OK);
    ASSERT_EQ(tops_run(PDK_OP_PROGRAM, 0, PDK_SOURCE_HOST, NULL)->status, PDK_OK);
    memcpy(master, fake_chip_mem(), 2u * chip->code_words);
}

/* The active package after a COPY. */
static uint16_t package_word(uint16_t addr)
{
    bool def;
    return pkg_slot_word(&op_pkg.reader, addr, &def);
}

TEST(keep_value_copies_the_master_as_it_is)
{
    make_master();
    const op_result_t *r = tops_copy(chip, NULL);   /* no ext: KEEP_VALUE, KEEP_VALUE, BandGap kept */
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(r->stage, PDK_STAGE_STORE);
    ASSERT_EQ(op_pkg_load(), PDK_OK);
    ASSERT_TRUE(op_pkg.hdr.flags & 0x100u);   /* CREATED_BY_COPY */
    ASSERT_EQ(op_pkg.hdr.image_words, chip->code_words);
    char name[40];
    snprintf(name, sizeof name, "copy PFS154 %08lX", (unsigned long)r->image_crc_norm);
    ASSERT_TRUE(!strcmp(op_pkg.hdr.name, name));
    ASSERT_EQ(op_pkg.plan.calib_count, 0);     /* no placeholder: the footprints stay */
    ASSERT_TRUE(op_pkg.plan.fuse_defined);
    ASSERT_EQ(op_pkg.plan.fuse, 0x3FFD);       /* from the last word */
    for (uint16_t a = 0; a < chip->code_words; a++)
        if (!chip_addr_reserved(chip, a))
            ASSERT_EQ(package_word(a), master[a]);
    ASSERT_EQ(r->calib_count, 3);
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(r->calib[i].site_status, PDK_SITE_COPIED);
    ASSERT_EQ(r->calib[0].master_value, 0x5A);
    ASSERT_TRUE(r->warnings & PDK_WARN_CALIB_VALUE_COPIED);
    ASSERT_TRUE(r->warnings & PDK_WARN_SERIAL_DUPLICATED);   /* the serial number went along */
    ASSERT_EQ(r->data_words, chip->code_words);
}

TEST(placeholders_by_mode)
{
    make_master();
    copy_ext_t e = {.mode_ihrc = 1, .mode_ilrc = 1, .bg_restore = 1, .target_ihrc = 7372800, .target_ilrc = 31250,
                    .mV = 3300, .name = "batch A"};
    const op_result_t *r = tops_copy(chip, &e);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(op_pkg_load(), PDK_OK);
    ASSERT_TRUE(!strcmp(op_pkg.hdr.name, "batch A"));
    ASSERT_EQ(op_pkg.plan.calib_count, 3);
    const calib_site_t *s = op_pkg.plan.calib;
    ASSERT_EQ(s[0].addr, IHRC);
    ASSERT_EQ(s[0].kind, CALIB_KIND_IHRC);
    ASSERT_EQ(s[0].target_hz, 7372800u);
    ASSERT_EQ(s[0].mV, 3300);
    ASSERT_EQ(s[0].reg0, chip->reg_ihrcr);
    ASSERT_EQ(s[1].target_hz, 31250u);
    ASSERT_EQ(s[2].kind, CALIB_KIND_BG);
    ASSERT_EQ(s[2].mV, 3950);
    ASSERT_EQ(s[2].reg0, chip->reg_bgtr);
    ASSERT_EQ(s[2].reg1, chip->reg_gpcc);
    ASSERT_EQ(r->calib[0].site_status, PDK_SITE_OK);
    ASSERT_EQ(r->calib[0].target_hz, 7372800u);
    ASSERT_EQ(r->calib[0].mode, 1);
}

TEST(snap_standard_encodes_the_master_value)
{
    make_master();
    copy_ext_t e = {.mode_ihrc = 2};
    const op_result_t *r = tops_copy(chip, &e);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_EQ(op_pkg_load(), PDK_OK);
    ASSERT_EQ(op_pkg.plan.calib_count, 1);
    ASSERT_EQ(op_pkg.plan.calib[0].target_hz, 0xFF00005Au);
    copy_ext_t bad = {.mode_ilrc = 2};   /* SNAP_STANDARD is for IHRC only */
    ASSERT_EQ(tops_copy(chip, &bad)->status, PDK_BAD_PAYLOAD);
    copy_ext_t no_target = {.mode_ihrc = 1};
    ASSERT_EQ(tops_copy(chip, &no_target)->status, PDK_BAD_PAYLOAD);
}

TEST(trim_write_outside_a_footprint_is_copied_with_a_warning)
{
    tops_put(&img, 0x20, (uint16_t)(calib_mov_a_imm(14) | 0x55));
    tops_put(&img, 0x21, calib_mov_io_a(14, chip->reg_ihrcr));
    make_master();
    const op_result_t *r = tops_copy(chip, NULL);
    ASSERT_EQ(r->status, PDK_OK);
    ASSERT_TRUE(r->warnings & PDK_WARN_CALIB_UNRECOGNIZED);
    ASSERT_EQ(r->fail_addr, 0x20);
    ASSERT_EQ(op_pkg_load(), PDK_OK);
    ASSERT_EQ(package_word(0x20), master[0x20]);   /* code as it is (FR-053) */
}

TEST(protected_and_read_only_chips)
{
    make_master();
    uint32_t crc = package_store_crc();
    fake_chip_mem()[0x7FF] = 0x3FFC;   /* read protection on */
    ASSERT_EQ(tops_copy(chip, NULL)->status, PDK_CHIP_PROTECTED);
    ASSERT_EQ(package_store_crc(), crc);   /* the stored package did not change */
    const chip_info_t *ro = tops_chip("PMS133");
    fake_chip_insert(ro);
    ASSERT_EQ(tops_copy(ro, NULL)->status, PDK_OP_UNSUPPORTED);
}
