/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supported PADAUK chips. Public part → CHIP_DESC (LIST_CHIPS); internal part (voltages, write/erase
 * parameters, trim register addresses) never leaves the programmer (principle I). */
#ifndef PDK_CHIP_TABLE_H
#define PDK_CHIP_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Protocol families */
enum {
    CHIP_OTP1_2 = 0x12,
    CHIP_OTP2_1 = 0x21,
    CHIP_OTP2_2 = 0x22,
    CHIP_OTP3_1 = 0x31,
    CHIP_FLASH_1 = 0xF1,
    CHIP_FLASH_2 = 0xF2,
};

#define CHIP_REG_UNKNOWN 0xFFu

typedef struct chip_info {
    const char *name;
    const char *variant1;
    const char *variant2;
    uint16_t id12;
    uint8_t family;
    uint8_t addr_bits;
    uint8_t code_bits;
    uint16_t code_words;
    uint16_t ram_bytes;
    bool excl_first;            /* address 0 excluded from blank check / verify */
    uint16_t excl_start;        /* reserved area, inclusive */
    uint16_t excl_end;          /* inclusive (clamped to code_words - 1 in CHIP_DESC) */
    /* voltages, mV (0 = operation not supported) */
    uint16_t vdd_cmd_read, vpp_cmd_read, vdd_read, vpp_read;
    uint16_t vdd_cmd_write, vpp_cmd_write, vdd_write, vpp_write;
    uint8_t write_block, write_groups, write_clocks;
    uint16_t vdd_cmd_erase, vpp_cmd_erase, vdd_erase, vpp_erase;
    uint8_t erase_clocks;
    /* IO addresses of trim/comparator registers (CHIP_REG_UNKNOWN if not known) */
    uint8_t reg_ihrcr, reg_ilrcr, reg_bgtr, reg_gpcc, reg_gpcs;
} chip_info_t;

size_t chip_count(void);
const chip_info_t *chip_at(size_t index);
const chip_info_t *chip_by_id(uint16_t id12);
/* Case-insensitive match against name and variants. */
bool chip_name_matches(const chip_info_t *c, const char *name);

bool chip_is_flash(const chip_info_t *c);
bool chip_can_write(const chip_info_t *c);
uint8_t chip_ops(const chip_info_t *c);
uint16_t chip_fuse_addr(const chip_info_t *c);
uint16_t chip_word_mask(const chip_info_t *c);
/* True if `addr` lies in the reserved area (the fuse word is never reported as reserved). */
bool chip_addr_reserved(const chip_info_t *c, uint16_t addr);

/* Size of one CHIP_DESC record and its encoder. */
#define CHIP_DESC_SIZE 48u
void chip_encode_desc(const chip_info_t *c, uint8_t index, uint8_t *out);

/* LIST_CHIPS handler */
uint16_t chip_cmd_list(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len);

#endif
