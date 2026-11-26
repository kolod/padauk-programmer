/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internals of the PADAUK chip engine (specs/002-chip-engine/data-model.md): the programming session and the family
 * sequences. Ported from the free-pdk firmware (easy-pdk-programmer-software/Firmware/source/Src/fpdk.c,
 * Copyright (C) 2019-2020 freepdk https://free-pdk.github.io, GPL-3.0-or-later) via docs/easy-pdk-programmer-protocol.md. */
#ifndef PDK_ENGINE_INTERNAL_H
#define PDK_ENGINE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "hal_icp.h"

/* ---- timings (free-pdk minimums, docs/easy-pdk-programmer-protocol.md §4) ---- */
#define ENG_BIT_CLOCK_NS 1000u           /* clock-high of one bit / an extra clock */
#define ENG_PROBE_VPP_MV 4500u
#define ENG_PROBE_VDD_MV 2000u
#define ENG_PAUSE_LEAVE_US 10000u        /* after leaving the mode */
#define ENG_PAUSE_LEAVE_HV_US 100000u    /* extra after erase / write */
#define ENG_PAUSE_LEAVE_CALIB_US 40000u  /* extra after calibration (free-pdk waits 50 ms with VDD off) */
#define ENG_READ_FLASH_CLK_US 5u         /* FLASH: CLK high before the first data bit */
#define ENG_POLL_WORDS 64u               /* poll() at least every 64 read words */
#define ENG_ERASE_HIGH_NS 5000000u       /* FLASH_1 erase clock high: 5 ms */
#define ENG_ERASE_HIGH_FLASH2_NS 40000000u   /* FLASH_2: 40 ms */
#define ENG_BLOCK_PAUSE_NS 100000u       /* after every write block: 100 µs */
#define ENG_OTP_PULSE_NS 30000u          /* OTP programming pulse on PA4: 30 µs high, 30 µs low */

/* ---- the programming session ---- */
typedef enum { SES_IDLE = 0, SES_OPEN, SES_HV } ses_state_t;
typedef enum { MODE_NONE = 0, MODE_PROBE, MODE_GETID, MODE_READ, MODE_WRITE, MODE_ERASE, MODE_CALIB, MODE_RUN } ses_mode_t;

typedef struct {
    ses_state_t state;
    ses_mode_t mode;
    const struct chip_info *chip;
    uint8_t family;
    uint32_t left_at_us;
    uint32_t pause_us;
} eng_session_t;

extern eng_session_t eng_ses;

/* Enter programming mode: waits the rest of the post-leave pause (polling), lines idle, VPP then VDD through
 * power_apply with the documented minimum pauses, family line setup. PDK_OK or a power fault / CANCELLED (left). */
uint16_t eng_enter(const struct chip_info *chip, uint8_t family, ses_mode_t mode, uint16_t vpp_mV, uint16_t vdd_mV,
                   chip_poll_fn poll);

/* Operation voltages: VDD then VPP with min_pause_us each (state → HV). */
uint16_t eng_raise(uint16_t vdd_mV, uint16_t vpp_mV, uint32_t min_pause_us);

/* VDD only (VPP 0), lines idle, for calibration and RUN; state OPEN in `mode`. */
uint16_t eng_supply(const struct chip_info *chip, ses_mode_t mode, uint16_t vdd_mV, chip_poll_fn poll);

/* Leave: lines idle, both supplies off at once, the next enter waits the pause. Never waits itself. */
void eng_leave(void);

/* True if the open session serves `chip` in `mode` (the next primitive may continue it). */
bool eng_continues(const struct chip_info *chip, ses_mode_t mode);

/* Wraps poll(): false → leave; the caller returns PDK_CANCELLED. */
bool eng_poll(chip_poll_fn poll);

/* Ends a primitive with an error: leave, return st. */
uint16_t eng_fail(uint16_t st);

/* ---- family sequences (family.c) ---- */
enum { CMD_READ = 0, CMD_WRITE, CMD_ERASE };

bool fam_is_flash(uint8_t family);
/* Line setup right after the supplies came up in enter(). */
void fam_lines(uint8_t family);
/* The command preamble on the family's data line. */
void fam_command(uint8_t family, uint8_t cmd);
/* FLASH: receives the ACK (16 / 17 bits), DAT back to output, one clock; returns the raw ACK. */
uint32_t fam_flash_ack(uint8_t family);
/* OTP: the ID bits after a write command (n bits on DAT, PA0 clock for OTP3_1). */
uint32_t fam_otp_id_bits(uint8_t family, uint8_t n);
/* Number of bits of the OTP GetID pass for a code width. */
uint8_t fam_otp_id_len(uint8_t family, uint8_t code_bits);
/* One word: address out, data in. */
uint16_t fam_read_word(const struct chip_info *chip, uint16_t addr);
/* One write block at an aligned address (FLASH_1: data then address; FLASH_2: address then data), with the
 * programming clocks. */
void fam_write_block(const struct chip_info *chip, uint16_t aligned_addr, const uint16_t *words);
/* One erase clock: CLK high for high_ns, then the short pulses of §4.8. */
void fam_erase_clock(uint32_t high_ns);

/* Checks the identity of `chip` for a session in `cmd` (FLASH: the ACK of the command in the open session; OTP: a
 * separate GetID pass before the session). PDK_OK, CHIP_NOT_FOUND or CHIP_ID_MISMATCH (session left). */
uint16_t eng_flash_check_ack(const struct chip_info *chip);
uint16_t eng_otp_getid(const struct chip_info *chip, uint16_t vpp_mV, uint16_t vdd_mV, chip_poll_fn poll);

#endif
