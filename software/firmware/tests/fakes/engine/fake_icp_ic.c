/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fake_icp_ic.h"

#include <string.h>

#define MAX_WORDS 0x1000u
#define V_MARGIN 300u   /* the operation needs its voltage within this margin */

typedef enum { PW_OFF = 0, PW_RUN, PW_PROG } pw_t;
typedef enum { PH_CMD = 0, PH_OTP3_ZEROS, PH_ACK, PH_READY } phase_t;
typedef enum { C_NONE = 0, C_READ, C_WRITE, C_ERASE, C_ANY } cmd_t;

static struct {
    const chip_info_t *chip;
    uint16_t id;
    uint16_t mem[MAX_WORDS];
    uint8_t written[MAX_WORDS];
    bool weak;
    uint16_t weak_addr;
    uint16_t vdd, vpp, max_vdd, max_vpp;
    pw_t pw;
    phase_t phase;
    cmd_t cmd;
    uint64_t stream;      /* answer bits, MSB first */
    uint8_t stream_len, stream_pos;
    uint16_t blk[16];
    uint8_t blk_n;
    bool have_addr, pending;
    uint16_t blk_addr;
    uint8_t prog_clocks, erase_clocks;
    bool clk_high, pa4_high;
    uint32_t clk_at, pa4_at;
    uint32_t enters, blocks, erases;
} m;

void fake_ic_reset(void) { memset(&m, 0, sizeof m); }

void fake_ic_insert(const chip_info_t *chip)
{
    m.chip = chip;
    m.id = chip->id12;
    for (unsigned i = 0; i < MAX_WORDS; i++)
        m.mem[i] = chip_word_mask(chip);
    memset(m.written, 0, sizeof m.written);
}

void fake_ic_remove(void) { m.chip = NULL; }
void fake_ic_set_id(uint16_t id12) { m.id = id12; }
uint16_t *fake_ic_mem(void) { return m.mem; }
uint32_t fake_ic_enters(void) { return m.enters; }
uint32_t fake_ic_blocks(void) { return m.blocks; }
uint32_t fake_ic_erases(void) { return m.erases; }
uint16_t fake_ic_max_vpp(void) { return m.max_vpp; }
uint16_t fake_ic_max_vdd(void) { return m.max_vdd; }

void fake_ic_weak(uint16_t addr)
{
    m.weak = true;
    m.weak_addr = addr;
}

static bool flash(void) { return m.chip->family == CHIP_FLASH_1 || m.chip->family == CHIP_FLASH_2; }
static icp_line_t data_in(void) { return flash() ? ICP_DAT : ICP_PA4; }

static void set_stream(uint64_t value, uint8_t len)
{
    m.stream = value;
    m.stream_len = len;
    m.stream_pos = 0;
}

/* The ID answer: (len − 12) leading bits, then the 12-bit ID, then zeros. */
static void id_stream(uint8_t len, uint8_t lead_pattern)
{
    set_stream(((uint64_t)lead_pattern << 12 | m.id) & ((1ull << len) - 1u), len);
}

static void start_block(void)
{
    m.blk_n = 0;
    m.have_addr = false;
    m.pending = false;
    m.prog_clocks = 0;
}

void fake_ic_power(bool vpp, uint16_t mV, uint32_t t_us)
{
    (void)t_us;
    if (vpp) {
        m.vpp = mV;
        if (mV > m.max_vpp)
            m.max_vpp = mV;
        return;
    }
    uint16_t was = m.vdd;
    m.vdd = mV;
    if (mV > m.max_vdd)
        m.max_vdd = mV;
    if (!mV) {
        m.pw = PW_OFF;
        return;
    }
    if (!was && m.chip) {
        if (m.vpp >= 3000) {   /* VPP before VDD: programming mode */
            m.pw = PW_PROG;
            m.phase = PH_CMD;
            m.cmd = C_NONE;
            m.enters++;
            start_block();
        } else {
            m.pw = PW_RUN;
        }
    }
}

static bool at_write_voltage(void)
{
    const chip_info_t *c = m.chip;
    return m.vdd + V_MARGIN >= c->vdd_write && m.vpp + V_MARGIN >= c->vpp_write;
}

static void commit(void)
{
    if (at_write_voltage()) {
        for (unsigned i = 0; i < m.chip->write_block; i++) {
            uint16_t a = (uint16_t)(m.blk_addr + i);
            if (a >= m.chip->code_words || (m.weak && a == m.weak_addr && m.written[a]))
                continue;
            if (m.blk[i] != chip_word_mask(m.chip))
                m.written[a] = 1;
            m.mem[a] &= m.blk[i];
        }
        m.blocks++;
    }
    start_block();
}

static void erase_clock(uint32_t high_us)
{
    const chip_info_t *c = m.chip;
    if (high_us < (c->family == CHIP_FLASH_2 ? 40000u : 5000u))
        return;
    if (m.vdd + V_MARGIN < c->vdd_erase || m.vpp + V_MARGIN < c->vpp_erase)
        return;
    if (++m.erase_clocks >= c->erase_clocks) {
        for (unsigned i = 0; i < MAX_WORDS; i++) {
            m.mem[i] = chip_word_mask(c);
            m.written[i] = 0;
        }
        m.erases++;
        m.erase_clocks = 0;
    }
}

void fake_ic_line(icp_line_t line, bool high, uint32_t t_us)
{
    if (!m.chip || m.pw != PW_PROG)
        return;
    if (line == ICP_PA4) {
        if (high) {
            m.pa4_at = t_us;
        } else if (m.pa4_high && m.clk_high && m.pending && t_us - m.pa4_at >= 30u) {
            m.prog_clocks++;   /* OTP programming pulse with CLK held high */
        }
        m.pa4_high = high;
        return;
    }
    if (line != ICP_CLK)
        return;
    if (high) {
        m.clk_at = t_us;
        m.clk_high = true;
        return;
    }
    uint32_t dur = t_us - m.clk_at;
    m.clk_high = false;
    if (m.cmd == C_ERASE) {
        erase_clock(dur);
        return;
    }
    if (m.cmd != C_WRITE || !m.pending)
        return;
    if (flash()) {
        if (dur >= (m.chip->family == CHIP_FLASH_2 ? 40u : 15u) && ++m.prog_clocks >= m.chip->write_clocks)
            commit();
    } else if (m.prog_clocks >= m.chip->write_clocks) {
        commit();   /* OTP: the clock falls after the pulses */
    }
}

static void command(uint32_t value)
{
    uint8_t fam = m.chip->family;
    uint32_t code = value & 0xFu;
    bool a5 = (value >> 4) == 0xA5A5A5Au, b5 = (value >> 4) == 0x5A5A5A5u;
    if (fam == CHIP_OTP3_1) {
        if (value == 0x5A5A5A5Au)
            m.phase = PH_OTP3_ZEROS;
        return;
    }
    if ((fam == CHIP_OTP2_1 || fam == CHIP_OTP2_2) ? !b5 : !a5)
        return;
    if (fam == CHIP_FLASH_2)
        m.cmd = code == 0xC ? C_READ : code == 0x7 ? C_WRITE : code == 0x5 ? C_ERASE : C_NONE;
    else
        m.cmd = code == 0x6 ? C_READ : code == 0x7 ? C_WRITE : code == 0x3 ? C_ERASE : C_NONE;
    if (flash()) {   /* a FLASH IC acknowledges any preamble (probe uses the FLASH_1 read code for every FLASH IC) */
        m.phase = PH_ACK;
        if (fam == CHIP_FLASH_2)
            id_stream(17, 0x15);
        else
            id_stream(16, 0xA);
        return;
    }
    if (m.cmd == C_NONE || m.cmd == C_ERASE)
        return;   /* OTP: no such command */
    m.phase = PH_READY;
    if (m.cmd == C_WRITE) {   /* an OTP write command answers the GetID bits until data arrives */
        uint8_t cb = m.chip->code_bits;
        id_stream(fam == CHIP_OTP2_1 ? (uint8_t)(cb + 12u) : (uint8_t)(2u * cb + 12u), 0);
    }
}

void fake_ic_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits)
{
    (void)clk;
    if (!m.chip || m.pw != PW_PROG)
        return;
    switch (m.phase) {
    case PH_CMD:
        if (bits == 32 && data == data_in())
            command(value);
        return;
    case PH_OTP3_ZEROS:
        if (bits == 27 && data == ICP_PA4 && value == 0) {
            m.phase = PH_READY;
            m.cmd = C_ANY;
            id_stream((uint8_t)(m.chip->code_bits + 13u), 0);
        }
        return;
    case PH_ACK:
        return;   /* the ACK was not read */
    case PH_READY: break;
    }
    if (data != data_in())
        return;   /* e.g. the OTP3_1 commit clock on PA7 */
    if (m.cmd == C_READ || m.cmd == C_ANY) {
        if (bits == m.chip->addr_bits)
            set_stream(value < m.chip->code_words ? m.mem[value] : chip_word_mask(m.chip), m.chip->code_bits);
        return;
    }
    if (m.cmd != C_WRITE)
        return;
    m.stream_len = 0;   /* data: no more ID bits */
    if (bits == m.chip->code_bits && m.blk_n < 16) {
        m.blk[m.blk_n++] = (uint16_t)value;
    } else if (bits == m.chip->addr_bits) {
        m.blk_addr = (uint16_t)value;
        m.have_addr = true;
    }
    m.pending = m.have_addr && m.blk_n >= m.chip->write_block;
    m.prog_clocks = 0;
}

uint32_t fake_ic_recv(icp_line_t data, icp_line_t clk, uint8_t bits)
{
    (void)clk;
    if (!m.chip || m.pw != PW_PROG || data != ICP_DAT)
        return 0;
    uint32_t v = 0;
    for (uint8_t i = 0; i < bits; i++) {
        uint32_t bit = 0;
        if (m.stream_pos < m.stream_len)
            bit = (uint32_t)(m.stream >> (m.stream_len - 1u - m.stream_pos)) & 1u;
        m.stream_pos++;
        v = v << 1 | bit;
    }
    if (m.phase == PH_ACK)
        m.phase = PH_READY;
    return v;
}
