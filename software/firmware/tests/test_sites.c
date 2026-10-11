/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Calibration and serial-number placeholder search (research R8). */
#include "pdk_test.h"

#include "chips/calib_sites.h"
#include "chips/serial_sites.h"
#include "proto/proto_defs.h"

#define MEM 4096

static uint16_t mem[MEM];
static bool def[MEM];

void pdk_test_setup(void)
{
    for (int i = 0; i < MEM; i++) {
        mem[i] = 0x0000;   /* NOP */
        def[i] = true;
    }
}

static uint16_t get(void *ctx, uint16_t a, bool *d)
{
    (void)ctx;
    *d = def[a];
    return def[a] ? mem[a] : 0xFFFF;
}

static void put_rc(uint8_t bits, uint16_t at, uint8_t kind, uint32_t hz, uint16_t mv, uint8_t reg)
{
    uint16_t op = calib_and_opcode(bits);
    uint8_t v[10] = {'R', 'C', kind, (uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16), (uint8_t)(hz >> 24),
                     (uint8_t)mv, (uint8_t)(mv >> 8), reg};
    for (int i = 0; i < 10; i++)
        mem[at + i] = op | v[i];
}

static void put_bg(uint8_t bits, uint16_t at, uint16_t mv, uint8_t bgtr, uint8_t gpcc, uint8_t gpcs)
{
    uint16_t op = calib_and_opcode(bits);
    uint8_t v[14] = {'B', 'G', 3, (uint8_t)mv, (uint8_t)(mv >> 8), bgtr, gpcc, gpcs, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 14; i++)
        mem[at + i] = op | v[i];
}

static void put_serial(uint8_t bits, uint16_t at)
{
    const char *m = "FPSERIAL";
    for (int i = 0; i < 8; i++)
        mem[at + i] = serial_ret_opcode(bits) | (uint8_t)m[i];
}

static calib_site_t sites[CALIB_MAX_SITES];
static uint8_t n;
static uint16_t bad;

TEST(opcodes_per_core_width)
{
    ASSERT_EQ(calib_and_opcode(13), 0x1400);
    ASSERT_EQ(calib_and_opcode(14), 0x2C00);
    ASSERT_EQ(calib_and_opcode(15), 0x5400);
    ASSERT_EQ(serial_ret_opcode(13), 0x0100);
    ASSERT_EQ(serial_ret_opcode(14), 0x0200);
    ASSERT_EQ(serial_ret_opcode(15), 0x0200);
    ASSERT_EQ(serial_ret_opcode(16), 0x0F00);
}

TEST(rc_and_bg_for_each_core_width)
{
    static const uint8_t widths[] = {13, 14, 15};
    for (int w = 0; w < 3; w++) {
        pdk_test_setup();
        put_rc(widths[w], 0x10, CALIB_KIND_IHRC, 8000000, 4000, 0x0B);
        put_rc(widths[w], 0x40, CALIB_KIND_ILRC, 95000, 3300, 0x39);
        put_bg(widths[w], 0x80, 4000, 0x1A, 0x18, 0x19);
        ASSERT_EQ(calib_find_sites(widths[w], get, NULL, 0x100, sites, &n, &bad), PDK_OK);
        ASSERT_EQ(n, 3);
        ASSERT_EQ(sites[0].addr, 0x10);
        ASSERT_EQ(sites[0].kind, CALIB_KIND_IHRC);
        ASSERT_EQ(sites[0].target_hz, 8000000);
        ASSERT_EQ(sites[0].mV, 4000);
        ASSERT_EQ(sites[0].reg0, 0x0B);
        ASSERT_EQ(sites[1].kind, CALIB_KIND_ILRC);
        ASSERT_EQ(sites[1].target_hz, 95000);
        ASSERT_EQ(sites[2].kind, CALIB_KIND_BG);
        ASSERT_EQ(sites[2].addr, 0x80);
        ASSERT_EQ(sites[2].len, 14);
        ASSERT_EQ(sites[2].reg0, 0x1A);
        ASSERT_EQ(sites[2].reg1, 0x18);
        ASSERT_EQ(sites[2].reg2, 0x19);
    }
}

TEST(wrong_width_does_not_match)
{
    put_rc(14, 0x10, CALIB_KIND_IHRC, 8000000, 4000, 0x0B);
    ASSERT_EQ(calib_find_sites(13, get, NULL, 0x100, sites, &n, &bad), PDK_OK);
    ASSERT_EQ(n, 0);
}

TEST(site_anywhere_including_the_end)
{
    put_rc(14, 0, CALIB_KIND_IHRC, 1000000, 5000, 0x0B);
    put_rc(14, 0x7F6 - 10, CALIB_KIND_IHRC, 1000000, 5000, 0x0B);
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x7F6, sites, &n, &bad), PDK_OK);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(sites[1].addr, 0x7F6 - 10);
}

TEST(site_cut_by_image_end_is_not_a_site)
{
    put_rc(14, 0x20, CALIB_KIND_IHRC, 1000000, 5000, 0x0B);
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x29, sites, &n, &bad), PDK_PKG_CALIB_UNKNOWN);
    ASSERT_EQ(bad, 0x20);
}

TEST(undefined_word_breaks_a_site)
{
    put_rc(14, 0x20, CALIB_KIND_IHRC, 1000000, 5000, 0x0B);
    def[0x25] = false;
    /* 5 words before the hole and 4 after it are two short runs: no site, not "unknown" either */
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x100, sites, &n, &bad), PDK_OK);
    ASSERT_EQ(n, 0);
}

TEST(sixteen_sites_ok_seventeen_rejected)
{
    for (int i = 0; i < 16; i++)
        put_rc(14, (uint16_t)(i * 12), CALIB_KIND_IHRC, 8000000, 4000, 0x0B);
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x400, sites, &n, &bad), PDK_OK);
    ASSERT_EQ(n, 16);
    put_rc(14, 16 * 12, CALIB_KIND_IHRC, 8000000, 4000, 0x0B);
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x400, sites, &n, &bad), PDK_BAD_PAYLOAD);
    ASSERT_EQ(bad, 16 * 12);
}

TEST(unknown_run_threshold)
{
    for (int i = 0; i < 5; i++)
        mem[0x30 + i] = 0x2C00 | 0x41;
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x100, sites, &n, &bad), PDK_OK);
    mem[0x35] = 0x2C00 | 0x41;
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x100, sites, &n, &bad), PDK_PKG_CALIB_UNKNOWN);
    ASSERT_EQ(bad, 0x30);
}

TEST(short_and_run_before_site_is_kept_apart)
{
    mem[0x0F] = 0x2C00 | 0x0F;   /* ordinary AND A,#0x0F right before the placeholder */
    put_rc(14, 0x10, CALIB_KIND_IHRC, 8000000, 4000, 0x0B);
    ASSERT_EQ(calib_find_sites(14, get, NULL, 0x100, sites, &n, &bad), PDK_OK);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(sites[0].addr, 0x10);
}

TEST(bg_with_nonzero_tail_is_unknown)
{
    put_bg(15, 0x10, 4000, 0x63, 0x2B, 0x2C);
    mem[0x10 + 13] = 0x5400 | 1;
    ASSERT_EQ(calib_find_sites(15, get, NULL, 0x100, sites, &n, &bad), PDK_PKG_CALIB_UNKNOWN);
    ASSERT_EQ(bad, 0x10);
}

TEST(sixteen_bit_core_has_no_rc_sites)
{
    for (int i = 0; i < 10; i++)
        mem[0x10 + i] = 0x1C00 | (uint16_t)"RC\1\0\0\0\0\0\0\0"[i];
    ASSERT_EQ(calib_find_sites(16, get, NULL, 0x100, sites, &n, &bad), PDK_PKG_CALIB_UNKNOWN);
}

TEST(serial_sites)
{
    put_serial(13, 0x00);
    put_serial(13, 0x20);
    put_serial(13, 0x3F8);
    ASSERT_EQ(serial_find_sites(13, get, NULL, 0x400, (uint16_t[4]){0}, 4), 3);
    uint16_t a[4];
    ASSERT_EQ(serial_find_sites(13, get, NULL, 0x400, a, 4), 3);
    ASSERT_EQ(a[0], 0x00);
    ASSERT_EQ(a[1], 0x20);
    ASSERT_EQ(a[2], 0x3F8);
    ASSERT_EQ(serial_find_sites(14, get, NULL, 0x400, a, 4), 0);   /* RET opcode differs */
    ASSERT_EQ(serial_find_sites(13, get, NULL, 0x3FF, a, 4), 2);   /* last one cut by the image end */
}

TEST(serial_site_needs_every_letter)
{
    put_serial(14, 0x10);
    mem[0x17] = 0x0200 | 'X';
    uint16_t a[4];
    ASSERT_EQ(serial_find_sites(14, get, NULL, 0x100, a, 4), 0);
    def[0x17] = false;
    ASSERT_EQ(serial_find_sites(14, get, NULL, 0x100, a, 4), 0);
}
