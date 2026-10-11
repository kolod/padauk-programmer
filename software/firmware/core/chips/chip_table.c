/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Chip table ported from easy-pdk-programmer-software/fpdkicdata.c
 * Copyright (C) 2019-2020 freepdk https://free-pdk.github.io (GPL-3.0-or-later).
 * Trim register addresses from easy-pdk-programmer-software/Examples/src/easypdk/<chip>.h.
 * Voltages converted to millivolts; write/erase parameters left at 0 for read-only chips.
 */
#include "chip_table.h"

#include <ctype.h>

#define U CHIP_REG_UNKNOWN

static const chip_info_t chips[] = {
    {.name = "PMS131", .variant1 = "PMC131", .id12 = 0xC83, .family = CHIP_OTP2_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x600, .ram_bytes = 0x58, .excl_start = 0x5F9, .excl_end = 0x600,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "PMS132", .variant1 = "PMS132B", .id12 = 0x109, .family = CHIP_OTP2_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x800, .ram_bytes = 0x80, .excl_start = 0x7F6, .excl_end = 0x800,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "PMS133", .variant1 = "PMS134", .id12 = 0xC19, .family = CHIP_OTP2_1, .addr_bits = 12, .code_bits = 15,
     .code_words = 0x1000, .ram_bytes = 0x100, .excl_start = 0xFF6, .excl_end = 0x1000,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "PMS150C", .variant1 = "PMS15A", .id12 = 0xA16, .family = CHIP_OTP1_2, .addr_bits = 12, .code_bits = 13,
     .code_words = 0x400, .ram_bytes = 0x40, .excl_start = 0x3F6, .excl_end = 0x400,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .vdd_cmd_write = 4300, .vpp_cmd_write = 8000, .vdd_write = 5800, .vpp_write = 10500,
     .write_block = 2, .write_groups = 1, .write_clocks = 8,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x1F, .reg_bgtr = 0x19, .reg_gpcc = 0x1A, .reg_gpcs = 0x1E},

    {.name = "PMS152", .id12 = 0xA27, .family = CHIP_OTP2_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x500, .ram_bytes = 0x50, .excl_start = 0x4F6, .excl_end = 0x500,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .vdd_cmd_write = 4300, .vpp_cmd_write = 8000, .vdd_write = 5800, .vpp_write = 10500,
     .write_block = 2, .write_groups = 1, .write_clocks = 8,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x39, .reg_bgtr = 0x1A, .reg_gpcc = 0x18, .reg_gpcs = 0x19},

    {.name = "PMS154B", .variant1 = "PMS154C", .id12 = 0xE06, .family = CHIP_OTP1_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x800, .ram_bytes = 0x80, .excl_first = true, .excl_start = 0x7E8, .excl_end = 0x800,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .vdd_cmd_write = 4300, .vpp_cmd_write = 8000, .vdd_write = 5800, .vpp_write = 11000,
     .write_block = 2, .write_groups = 1, .write_clocks = 8,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "PFS154", .id12 = 0xAA1, .family = CHIP_FLASH_1, .addr_bits = 13, .code_bits = 14,
     .code_words = 0x800, .ram_bytes = 0x80, .excl_start = 0x7E0, .excl_end = 0x7F0,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5500, .vdd_read = 2500, .vpp_read = 5500,
     .vdd_cmd_write = 2500, .vpp_cmd_write = 5500, .vdd_write = 5800, .vpp_write = 8500,
     .write_block = 4, .write_groups = 1, .write_clocks = 8,
     .vdd_cmd_erase = 2500, .vpp_cmd_erase = 5500, .vdd_erase = 3000, .vpp_erase = 9000, .erase_clocks = 2,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x39, .reg_bgtr = 0x1A, .reg_gpcc = 0x18, .reg_gpcs = 0x19},

    {.name = "PMS171B", .id12 = 0xD36, .family = CHIP_OTP2_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x600, .ram_bytes = 0x60, .excl_start = 0x5F6, .excl_end = 0x600,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .vdd_cmd_write = 4300, .vpp_cmd_write = 8000, .vdd_write = 5800, .vpp_write = 10500,
     .write_block = 2, .write_groups = 1, .write_clocks = 8,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x39, .reg_bgtr = 0x1A, .reg_gpcc = 0x18, .reg_gpcs = 0x19},

    {.name = "PFS172", .id12 = 0xCA6, .family = CHIP_FLASH_2, .addr_bits = 13, .code_bits = 14,
     .code_words = 0x800, .ram_bytes = 0x80, .excl_start = 0x7E0, .excl_end = 0x7F0,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5500, .vdd_read = 2500, .vpp_read = 0,
     .vdd_cmd_write = 2500, .vpp_cmd_write = 5500, .vdd_write = 5300, .vpp_write = 0,
     .write_block = 4, .write_groups = 1, .write_clocks = 4,
     .vdd_cmd_erase = 2500, .vpp_cmd_erase = 5500, .vdd_erase = 5300, .vpp_erase = 0, .erase_clocks = 14,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x3B, .reg_bgtr = 0x3C, .reg_gpcc = 0x2B, .reg_gpcs = 0x2C},

    {.name = "PFS173", .id12 = 0xEA2, .family = CHIP_FLASH_1, .addr_bits = 13, .code_bits = 15,
     .code_words = 0xC00, .ram_bytes = 0x100, .excl_start = 0xBE0, .excl_end = 0xBF0,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5500, .vdd_read = 2500, .vpp_read = 5500,
     .vdd_cmd_write = 2500, .vpp_cmd_write = 6500, .vdd_write = 5800, .vpp_write = 9000,
     .write_block = 4, .write_groups = 1, .write_clocks = 8,
     .vdd_cmd_erase = 2500, .vpp_cmd_erase = 5500, .vdd_erase = 3000, .vpp_erase = 9000, .erase_clocks = 4,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x62, .reg_bgtr = 0x63, .reg_gpcc = 0x2B, .reg_gpcs = 0x2C},

    {.name = "PMC251", .id12 = 0x058, .family = CHIP_OTP3_1, .addr_bits = 13, .code_bits = 16,
     .code_words = 0x400, .ram_bytes = 0x3B, .excl_start = 0x3F9, .excl_end = 0x400,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5000, .vdd_read = 2500, .vpp_read = 5500,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "PMS271", .variant1 = "PMC271", .id12 = 0xA58, .family = CHIP_OTP3_1, .addr_bits = 13, .code_bits = 16,
     .code_words = 0x400, .ram_bytes = 0x40, .excl_start = 0x3F9, .excl_end = 0x400,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5000, .vdd_read = 2500, .vpp_read = 5500,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

    {.name = "MCU390", .id12 = 0xC31, .family = CHIP_OTP1_2, .addr_bits = 12, .code_bits = 14,
     .code_words = 0x800, .ram_bytes = 0x80, .excl_start = 0x700, .excl_end = 0x800,
     .vdd_cmd_read = 3000, .vpp_cmd_read = 5000, .vdd_read = 3000, .vpp_read = 5000,
     .reg_ihrcr = U, .reg_ilrcr = U, .reg_bgtr = U, .reg_gpcc = U, .reg_gpcs = U},

#ifdef PDK_TEST_EXTRA_CHIP
    /* Test-only entry (SC-006): a chip unknown to any client build. */
    {.name = "PTEST1", .id12 = 0x7E5, .family = CHIP_FLASH_1, .addr_bits = 13, .code_bits = 14,
     .code_words = 0x400, .ram_bytes = 0x40, .excl_start = 0x3E0, .excl_end = 0x3F0,
     .vdd_cmd_read = 2500, .vpp_cmd_read = 5500, .vdd_read = 2500, .vpp_read = 5500,
     .vdd_cmd_write = 2500, .vpp_cmd_write = 5500, .vdd_write = 5800, .vpp_write = 8500,
     .write_block = 4, .write_groups = 1, .write_clocks = 8,
     .vdd_cmd_erase = 2500, .vpp_cmd_erase = 5500, .vdd_erase = 3000, .vpp_erase = 9000, .erase_clocks = 2,
     .reg_ihrcr = 0x0B, .reg_ilrcr = 0x39, .reg_bgtr = 0x1A, .reg_gpcc = 0x18, .reg_gpcs = 0x19},
#endif
};

size_t chip_count(void) { return sizeof chips / sizeof chips[0]; }

const chip_info_t *chip_at(size_t index) { return index < chip_count() ? &chips[index] : NULL; }

const chip_info_t *chip_by_id(uint16_t id12)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chips[i].id12 == (id12 & 0xFFFu))
            return &chips[i];
    return NULL;
}

static bool name_eq(const char *a, const char *b)
{
    if (!a || !b || !*a)
        return false;
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
        a++, b++;
    }
    return *a == *b;
}

bool chip_name_matches(const chip_info_t *c, const char *name)
{
    return name_eq(c->name, name) || name_eq(c->variant1, name) || name_eq(c->variant2, name);
}

bool chip_is_flash(const chip_info_t *c) { return (c->family & 0xF0u) == 0xF0u; }

bool chip_can_write(const chip_info_t *c) { return c->vdd_cmd_write && c->vdd_write; }

uint8_t chip_ops(const chip_info_t *c)
{
    uint8_t ops = 0;
    if (c->vdd_cmd_read)
        ops |= 1u << 0;
    if (chip_can_write(c))
        ops |= 1u << 1;
    if (chip_is_flash(c) && c->vdd_cmd_erase && c->vdd_erase)
        ops |= 1u << 2;
    if (chip_can_write(c) && c->code_bits >= 13 && c->code_bits <= 15)
        ops |= 1u << 3;
    return ops;
}

uint16_t chip_fuse_addr(const chip_info_t *c) { return (uint16_t)(c->code_words - 1u); }

uint16_t chip_word_mask(const chip_info_t *c) { return (uint16_t)((1u << c->code_bits) - 1u); }

bool chip_addr_reserved(const chip_info_t *c, uint16_t addr)
{
    if (addr == chip_fuse_addr(c))
        return false;
    return c->excl_end > c->excl_start && addr >= c->excl_start && addr <= c->excl_end;
}

void chip_encode_desc(const chip_info_t *c, uint8_t index, uint8_t *out)
{
    uint16_t res_end = c->excl_end;
    if (res_end > c->code_words - 1u)
        res_end = (uint16_t)(c->code_words - 1u);
    bool has_res = c->excl_end > c->excl_start;

    out[0] = index;
    out[1] = (uint8_t)c->id12;
    out[2] = (uint8_t)(c->id12 >> 8);
    out[3] = c->family;
    out[4] = (uint8_t)c->code_words;
    out[5] = (uint8_t)(c->code_words >> 8);
    out[6] = c->code_bits;
    out[7] = (uint8_t)c->ram_bytes;
    out[8] = (uint8_t)(c->ram_bytes >> 8);
    out[9] = chip_ops(c);
    uint16_t rs = has_res ? c->excl_start : 0xFFFFu, re = has_res ? res_end : 0xFFFFu;
    out[10] = (uint8_t)rs;
    out[11] = (uint8_t)(rs >> 8);
    out[12] = (uint8_t)re;
    out[13] = (uint8_t)(re >> 8);
    out[14] = c->excl_first ? 1 : 0;

    char *names = (char *)out + 15;   /* 33 bytes, NUL padded */
    size_t pos = 0;
    const char *list[3] = {c->name, c->variant1, c->variant2};
    for (int i = 0; i < 3; i++) {
        const char *n = list[i];
        if (!n || !*n)
            continue;
        if (pos && pos < 32)
            names[pos++] = '/';
        while (*n && pos < 32)
            names[pos++] = *n++;
    }
    while (pos < 33)
        names[pos++] = 0;
}
