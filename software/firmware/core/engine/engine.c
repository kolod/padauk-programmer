/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PADAUK chip engine: the programming session and the read primitive (specs/002-chip-engine, research R4–R6).
 *
 * A session stays open while consecutive primitives need the same mode on the same IC (block-by-block writes would
 * otherwise enter and leave per block, research R4). Leaving removes the supplies at once; the mandatory pause after
 * leaving is charged to the next enter, so the safe state is immediate. Every voltage goes through power_apply()
 * (feature 001 FR-040) with the free-pdk pause as the minimum; the ID is checked in every session before any operation
 * voltage (FLASH: the command ACK; OTP: a GetID pass). */
#include <string.h>

#include "engine_internal.h"
#include "hal_power.h"
#include "hal_time.h"
#include "power/power.h"
#include "proto/proto_defs.h"
#include "proto/trace.h"

eng_session_t eng_ses;

/* ---------------------------------------------------------------- session */

static uint16_t wait_pause(chip_poll_fn poll)
{
    if (!eng_ses.pause_us)
        return PDK_OK;
    while (hal_time_us() - eng_ses.left_at_us < eng_ses.pause_us) {
        if (poll && !poll())
            return PDK_CANCELLED;
        hal_delay_us(1000);
    }
    eng_ses.pause_us = 0;
    return PDK_OK;
}

uint16_t eng_enter(const struct chip_info *chip, uint8_t family, ses_mode_t mode, uint16_t vpp_mV, uint16_t vdd_mV,
                   chip_poll_fn poll)
{
    if (eng_ses.state != SES_IDLE)
        eng_leave();
    uint16_t st = wait_pause(poll);
    if (st != PDK_OK)
        return st;
    hal_icp_idle();
    hal_icp_mode(ICP_CLK, ICP_OUT_LOW);
    eng_ses.chip = chip;
    eng_ses.family = family;
    eng_ses.mode = mode;
    eng_ses.state = SES_OPEN;
    if ((st = power_apply(PDK_RAIL_VPP, vpp_mV, POWER_PAUSE_VPP_CMD_US, NULL)) != PDK_OK ||
        (st = power_apply(PDK_RAIL_VDD, vdd_mV, POWER_PAUSE_VDD_CMD_US, NULL)) != PDK_OK)
        return eng_fail(st);
    fam_lines(family);
    trace_emit(TRACE_ENTER, family, vpp_mV, vdd_mV, mode);
    return PDK_OK;
}

uint16_t eng_raise(uint16_t vdd_mV, uint16_t vpp_mV, uint32_t min_pause_us)
{
    uint16_t st;
    if ((st = power_apply(PDK_RAIL_VDD, vdd_mV, min_pause_us, NULL)) != PDK_OK ||
        (st = power_apply(PDK_RAIL_VPP, vpp_mV, min_pause_us, NULL)) != PDK_OK)
        return eng_fail(st);
    eng_ses.state = SES_HV;
    return PDK_OK;
}

void eng_leave(void)
{
    bool was_open = eng_ses.state != SES_IDLE;
    bool hv_op = eng_ses.mode == MODE_WRITE || eng_ses.mode == MODE_ERASE;
    hal_icp_idle();        /* lines released before the supplies drop */
    hal_power_off_all();
    if (was_open) {
        eng_ses.left_at_us = hal_time_us();
        eng_ses.pause_us = ENG_PAUSE_LEAVE_US + (hv_op ? ENG_PAUSE_LEAVE_HV_US : 0u);
        trace_emit(TRACE_LEAVE, (uint8_t)eng_ses.mode, (uint16_t)(eng_ses.pause_us / 1000u), 0, 0);
    }
    eng_ses.state = SES_IDLE;
    eng_ses.mode = MODE_NONE;
    eng_ses.chip = NULL;
}

bool eng_continues(const struct chip_info *chip, ses_mode_t mode)
{
    return eng_ses.state == SES_HV && eng_ses.chip == chip && eng_ses.mode == mode;
}

bool eng_poll(chip_poll_fn poll)
{
    if (!poll || poll())
        return true;
    eng_leave();
    return false;
}

uint16_t eng_fail(uint16_t st)
{
    eng_leave();
    return st;
}

void chip_engine_safe_state(void) { eng_leave(); }

/* ---------------------------------------------------------------- identity */

uint16_t eng_flash_check_ack(const struct chip_info *chip)
{
    uint32_t ack = fam_flash_ack(chip->family);
    trace_emit(TRACE_ACK, chip->family, chip->id12, chip->family == CHIP_FLASH_2 ? 17 : 16, ack);
    if ((ack & 0xFFFu) == chip->id12)
        return PDK_OK;
    return eng_fail(ack == 0 ? PDK_CHIP_NOT_FOUND : PDK_CHIP_ID_MISMATCH);
}

uint16_t eng_otp_getid(const struct chip_info *chip, uint16_t vpp_mV, uint16_t vdd_mV, chip_poll_fn poll)
{
    uint16_t st = eng_enter(chip, chip->family, MODE_GETID, vpp_mV, vdd_mV, poll);
    if (st != PDK_OK)
        return st;
    fam_command(chip->family, CMD_WRITE);
    uint8_t n = fam_otp_id_len(chip->family, chip->code_bits);
    uint32_t raw = fam_otp_id_bits(chip->family, n), id = raw & 0xFFFu;
    trace_emit(TRACE_GETID, chip->family, chip->id12, n, raw);
    eng_leave();   /* before any programming voltage */
    if (id == chip->id12)
        return PDK_OK;
    return id == 0 ? PDK_CHIP_NOT_FOUND : PDK_CHIP_ID_MISMATCH;
}

/* Opens a session of `chip` for `cmd`: OTP GetID, enter at the command voltages, command, FLASH ACK, operation voltages. */
static uint16_t open_session(const struct chip_info *chip, ses_mode_t mode, uint8_t cmd, uint16_t vpp_cmd,
                             uint16_t vdd_cmd, uint16_t vdd_hv, uint16_t vpp_hv, uint32_t raise_us, chip_poll_fn poll)
{
    uint16_t st;
    bool flash = fam_is_flash(chip->family);
    if (!flash && (st = eng_otp_getid(chip, vpp_cmd, vdd_cmd, poll)) != PDK_OK)
        return st;
    if ((st = eng_enter(chip, chip->family, mode, vpp_cmd, vdd_cmd, poll)) != PDK_OK)
        return st;
    fam_command(chip->family, cmd);
    if (flash && (st = eng_flash_check_ack(chip)) != PDK_OK)
        return st;
    if (vdd_hv != vdd_cmd || vpp_hv != vpp_cmd)
        return eng_raise(vdd_hv, vpp_hv, raise_us);
    eng_ses.state = SES_HV;
    return PDK_OK;
}

/* ---------------------------------------------------------------- read */

uint16_t chip_engine_read(const struct chip_info *chip, uint16_t addr, uint16_t count, chip_sink_fn sink, void *ctx,
                          chip_poll_fn poll)
{
    uint16_t st;
    if (!eng_continues(chip, MODE_READ)) {
        eng_leave();
        if ((st = open_session(chip, MODE_READ, CMD_READ, chip->vpp_cmd_read, chip->vdd_cmd_read, chip->vdd_read,
                               chip->vpp_read, POWER_PAUSE_READ_US, poll)) != PDK_OK)
            return st;
    }
    uint16_t mask = chip_word_mask(chip);
    for (uint32_t i = 0; i < count; i++) {
        if (i % ENG_POLL_WORDS == 0 && !eng_poll(poll))
            return PDK_CANCELLED;
        uint16_t a = (uint16_t)(addr + i);
        sink(ctx, a, (uint16_t)(fam_read_word(chip, a) & mask));
    }
    return PDK_OK;
}

/* ---------------------------------------------------------------- not yet implemented (later stories) */

/* ---------------------------------------------------------------- erase (FLASH, §4.8) */

uint16_t chip_engine_erase(const struct chip_info *chip, chip_poll_fn poll)
{
    if (!fam_is_flash(chip->family) || !chip->erase_clocks)
        return eng_fail(PDK_OP_UNSUPPORTED);   /* before any voltage */
    eng_leave();
    uint16_t st = open_session(chip, MODE_ERASE, CMD_ERASE, chip->vpp_cmd_erase, chip->vdd_cmd_erase, chip->vdd_erase,
                               chip->vpp_erase, POWER_PAUSE_HV_US, poll);
    if (st != PDK_OK)
        return st;
    uint32_t high_ns = chip->family == CHIP_FLASH_2 ? ENG_ERASE_HIGH_FLASH2_NS : ENG_ERASE_HIGH_NS;
    for (uint8_t i = 0; i < chip->erase_clocks; i++) {
        if (!eng_poll(poll))
            return PDK_CANCELLED;
        fam_erase_clock(high_ns);
    }
    hal_icp_clock(ENG_BIT_CLOCK_NS);
    eng_leave();   /* the next enter waits 10 ms + 100 ms */
    return PDK_OK;
}

/* ---------------------------------------------------------------- write (§4.9) */

uint16_t chip_engine_write_block(const struct chip_info *chip, uint16_t addr, const uint16_t *words, uint8_t count,
                                 chip_poll_fn poll)
{
    if (!chip_can_write(chip) || !fam_is_flash(chip->family) || count != chip->write_block || count > 16)
        return eng_fail(PDK_OP_UNSUPPORTED);   /* OTP writing: user story 4 */
    uint16_t st;
    if (!eng_continues(chip, MODE_WRITE)) {   /* consecutive blocks share one session (research R4) */
        eng_leave();
        if ((st = open_session(chip, MODE_WRITE, CMD_WRITE, chip->vpp_cmd_write, chip->vdd_cmd_write, chip->vdd_write,
                               chip->vpp_write, POWER_PAUSE_HV_US, poll)) != PDK_OK)
            return st;
    }
    if (!eng_poll(poll))
        return PDK_CANCELLED;
    fam_write_block(chip, (uint16_t)(addr - addr % count), words);
    hal_icp_delay_ns(ENG_BLOCK_PAUSE_NS);
    return PDK_OK;
}

uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint32_t target_hz,
                               uint16_t mV, uint8_t *value, uint32_t *measured_hz, chip_poll_fn poll)
{
    (void)chip, (void)site_addr, (void)kind, (void)target_hz, (void)mV, (void)poll;
    *value = 0;
    *measured_hz = 0;
    return eng_fail(PDK_OP_UNSUPPORTED);
}

uint16_t chip_engine_measure_freq(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint8_t start_value,
                                  uint16_t mV, uint32_t *hz, chip_poll_fn poll)
{
    (void)chip, (void)site_addr, (void)kind, (void)start_value, (void)mV, (void)poll;
    *hz = 0;
    return eng_fail(PDK_OP_UNSUPPORTED);
}

uint16_t chip_engine_run_start(uint16_t vdd_mV)
{
    (void)vdd_mV;
    return eng_fail(PDK_OP_UNSUPPORTED);
}

void chip_engine_run_stop(void) { eng_leave(); }
