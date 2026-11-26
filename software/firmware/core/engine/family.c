/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Family sequences of the PADAUK programming protocol (docs/easy-pdk-programmer-protocol.md §4.3–§4.7), ported from the
 * free-pdk firmware (fpdk.c, Copyright (C) 2019-2020 freepdk https://free-pdk.github.io, GPL-3.0-or-later).
 *
 *   family   data → IC   data ← IC   preamble                 ID
 *   FLASH_1  PA6         PA6         0xA5A5A5A0 | cmd         16-bit ACK
 *   FLASH_2  PA6         PA6         0xA5A5A5A0 | cmd         17-bit ACK
 *   OTP1_2   PA4         PA6         0xA5A5A5A0 | cmd         GetID: 2·bits + 12
 *   OTP2_1   PA4         PA6         0x5A5A5A50 | cmd         GetID: bits + 12
 *   OTP2_2   PA4         PA6         0x5A5A5A50 | cmd         GetID: 2·bits + 12
 *   OTP3_1   PA4 (+PA0)  PA6 (PA0)   0x5A5A5A5A + 27 zeros    GetID: bits + 13, PA0 clock */
#include "engine_internal.h"

bool fam_is_flash(uint8_t family) { return family == CHIP_FLASH_1 || family == CHIP_FLASH_2; }

void fam_lines(uint8_t family)
{
    if (fam_is_flash(family)) {
        hal_icp_mode(ICP_DAT, ICP_OUT_LOW);
        return;
    }
    hal_icp_mode(ICP_DAT, ICP_IN_PULLDOWN);
    hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
    if (family == CHIP_OTP3_1) {
        hal_icp_mode(ICP_PA0, ICP_OUT_LOW);
        hal_icp_mode(ICP_PA7, ICP_OUT_LOW);
    }
}

static uint8_t cmd_code(uint8_t family, uint8_t cmd)
{
    static const uint8_t normal[] = {0x6, 0x7, 0x3}, flash2[] = {0xC, 0x7, 0x5};
    return family == CHIP_FLASH_2 ? flash2[cmd] : normal[cmd];
}

void fam_command(uint8_t family, uint8_t cmd)
{
    switch (family) {
    case CHIP_FLASH_1:
    case CHIP_FLASH_2:
        hal_icp_send(ICP_DAT, ICP_CLK, 0xA5A5A5A0u | cmd_code(family, cmd), 32);
        break;
    case CHIP_OTP1_2:
        hal_icp_send(ICP_PA4, ICP_CLK, 0xA5A5A5A0u | cmd_code(family, cmd), 32);
        hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
        break;
    case CHIP_OTP2_1:
    case CHIP_OTP2_2:
        hal_icp_send(ICP_PA4, ICP_CLK, 0x5A5A5A50u | cmd_code(family, cmd), 32);
        hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
        break;
    case CHIP_OTP3_1:   /* no command code: the operation follows from what comes next */
        hal_icp_send(ICP_PA4, ICP_CLK, 0x5A5A5A5Au, 32);
        hal_icp_send(ICP_PA4, ICP_CLK, 0, 27);
        hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
        break;
    default: break;
    }
}

uint32_t fam_flash_ack(uint8_t family)
{
    hal_icp_mode(ICP_DAT, ICP_IN);
    uint32_t ack = hal_icp_recv(ICP_DAT, ICP_CLK, family == CHIP_FLASH_2 ? 17 : 16);
    hal_icp_mode(ICP_DAT, ICP_OUT_LOW);
    hal_icp_clock(ENG_BIT_CLOCK_NS);
    return ack;
}

uint8_t fam_otp_id_len(uint8_t family, uint8_t code_bits)
{
    switch (family) {
    case CHIP_OTP2_1: return (uint8_t)(code_bits + 12u);
    case CHIP_OTP3_1: return (uint8_t)(code_bits + 13u);
    default: return (uint8_t)(2u * code_bits + 12u);
    }
}

uint32_t fam_otp_id_bits(uint8_t family, uint8_t n)
{
    icp_line_t clk = family == CHIP_OTP3_1 ? ICP_PA0 : ICP_CLK;
    uint32_t v = 0;
    while (n > 32) {   /* keep the last 32 bits, as the free-pdk 32-bit accumulator does */
        uint8_t k = n - 32 > 32 ? 32 : (uint8_t)(n - 32);
        (void)hal_icp_recv(ICP_DAT, clk, k);
        n = (uint8_t)(n - k);
    }
    v = hal_icp_recv(ICP_DAT, clk, n);
    return v;
}

uint16_t fam_read_word(const struct chip_info *chip, uint16_t addr)
{
    uint32_t w;
    switch (chip->family) {
    case CHIP_FLASH_1:
    case CHIP_FLASH_2:
        hal_icp_send(ICP_DAT, ICP_CLK, addr, chip->addr_bits);
        hal_icp_mode(ICP_DAT, ICP_IN);
        hal_icp_mode(ICP_CLK, ICP_OUT_HIGH);   /* CLK high ≥ 5 µs before the first data bit */
        hal_icp_delay_ns(ENG_READ_FLASH_CLK_US * 1000u);
        w = hal_icp_recv(ICP_DAT, ICP_CLK, chip->code_bits);
        hal_icp_mode(ICP_DAT, ICP_OUT_LOW);
        hal_icp_clock(ENG_BIT_CLOCK_NS);
        break;
    case CHIP_OTP3_1:
        hal_icp_send(ICP_PA4, ICP_PA0, addr, chip->addr_bits);
        hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
        hal_icp_mode(ICP_PA7, ICP_OUT_HIGH);   /* commit: PA7 pulse ≥ 1 µs + one PA0 clock */
        hal_icp_delay_ns(ENG_BIT_CLOCK_NS);
        hal_icp_mode(ICP_PA7, ICP_OUT_LOW);
        hal_icp_send(ICP_PA7, ICP_PA0, 0, 1);
        w = hal_icp_recv(ICP_DAT, ICP_PA0, chip->code_bits);
        break;
    default:   /* OTP1_2, OTP2_1, OTP2_2 */
        hal_icp_send(ICP_PA4, ICP_CLK, addr, chip->addr_bits);
        hal_icp_mode(ICP_PA4, ICP_OUT_LOW);
        w = hal_icp_recv(ICP_DAT, ICP_CLK, chip->code_bits);
        break;
    }
    return (uint16_t)w;
}

/* §4.9 WriteBlock. FLASH_1 (PFS154, PFS173): data words, address, DAT input, 4 µs, groups × (clocks × 15 µs / 15 µs,
 * one extra clock, 4 µs), DAT output, 25 µs. FLASH_2 (PFS172): address first, then the data words, DAT input,
 * groups × (an extra clock, 2 µs, clocks × 40 µs / 40 µs, 4 × 2 µs / 2 µs), DAT output, 25 µs. */
void fam_write_block(const struct chip_info *chip, uint16_t aligned_addr, const uint16_t *words)
{
    uint8_t n = chip->write_block, groups = chip->write_groups ? chip->write_groups : 1u;
    if (chip->family == CHIP_FLASH_2) {
        hal_icp_send(ICP_DAT, ICP_CLK, aligned_addr, chip->addr_bits);
        for (uint8_t i = 0; i < n; i++)
            hal_icp_send(ICP_DAT, ICP_CLK, words[i], chip->code_bits);
        hal_icp_mode(ICP_DAT, ICP_IN);
        for (uint8_t g = 0; g < groups; g++) {
            hal_icp_clock(ENG_BIT_CLOCK_NS);
            hal_icp_delay_ns(2000u);
            for (uint8_t c = 0; c < chip->write_clocks; c++) {
                hal_icp_clock(40000u);
                hal_icp_delay_ns(40000u);
            }
            for (uint8_t c = 0; c < 4; c++) {
                hal_icp_clock(2000u);
                hal_icp_delay_ns(2000u);
            }
        }
    } else {
        for (uint8_t i = 0; i < n; i++)
            hal_icp_send(ICP_DAT, ICP_CLK, words[i], chip->code_bits);
        hal_icp_send(ICP_DAT, ICP_CLK, aligned_addr, chip->addr_bits);
        hal_icp_mode(ICP_DAT, ICP_IN);
        hal_icp_delay_ns(4000u);
        for (uint8_t g = 0; g < groups; g++) {
            for (uint8_t c = 0; c < chip->write_clocks; c++) {
                hal_icp_clock(15000u);
                hal_icp_delay_ns(15000u);
            }
            hal_icp_clock(ENG_BIT_CLOCK_NS);
            hal_icp_delay_ns(4000u);
        }
    }
    hal_icp_mode(ICP_DAT, ICP_OUT_LOW);
    hal_icp_delay_ns(25000u);
}

/* §4.8: CLK high (5 ms; FLASH_2 40 ms), low 1 µs, high 1 µs, low 4 µs. */
void fam_erase_clock(uint32_t high_ns)
{
    hal_icp_mode(ICP_CLK, ICP_OUT_HIGH);
    hal_icp_delay_ns(high_ns);
    hal_icp_mode(ICP_CLK, ICP_OUT_LOW);
    hal_icp_delay_ns(1000u);
    hal_icp_mode(ICP_CLK, ICP_OUT_HIGH);
    hal_icp_delay_ns(1000u);
    hal_icp_mode(ICP_CLK, ICP_OUT_LOW);
    hal_icp_delay_ns(4000u);
}
