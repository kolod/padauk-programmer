/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fake_chip.h"

#include <string.h>

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "hal_power.h"
#include "power/power.h"
#include "proto/proto_defs.h"

#define MAX_WORDS 0x1000u
#define MAX_SCRIPT 16u
#define READ_POLL_EVERY 64u

static struct {
    const chip_info_t *chip;
    uint16_t id12;
    uint16_t mem[MAX_WORDS];
    uint8_t written[MAX_WORDS];   /* writes per word, for the weak cell */
    fake_cal_t script[MAX_SCRIPT];
    uint8_t script_value[MAX_SCRIPT];
    unsigned script_len, script_pos;
    uint16_t weak_addr;
    bool weak;
    void (*hook)(void);
    uint32_t writes, erases, safe, calibrations;
    uint32_t measure_hz;   /* what measure_freq reports (0 = no signal) */
} c;

void fake_chip_reset(void) { memset(&c, 0, sizeof c); }

void fake_chip_insert(const chip_info_t *chip)
{
    c.chip = chip;
    c.id12 = chip->id12;
    for (unsigned i = 0; i < MAX_WORDS; i++)
        c.mem[i] = chip_word_mask(chip);
    memset(c.written, 0, sizeof c.written);
}

void fake_chip_remove(void) { c.chip = NULL; }
void fake_chip_set_id(uint16_t id12) { c.id12 = id12; }
uint16_t *fake_chip_mem(void) { return c.mem; }

void fake_chip_calib(fake_cal_t outcome, uint8_t value)
{
    if (c.script_len < MAX_SCRIPT) {
        c.script[c.script_len] = outcome;
        c.script_value[c.script_len++] = value;
    }
}

void fake_chip_weak(uint16_t addr)
{
    c.weak = true;
    c.weak_addr = addr;
}

void fake_chip_hook(void (*fn)(void)) { c.hook = fn; }
void fake_chip_measure(uint32_t hz) { c.measure_hz = hz; }
uint32_t fake_chip_writes(void) { return c.writes; }
uint32_t fake_chip_erases(void) { return c.erases; }
uint32_t fake_chip_safe_states(void) { return c.safe; }
uint32_t fake_chip_calibrations(void) { return c.calibrations; }

static bool poll_now(chip_poll_fn poll)
{
    if (c.hook)
        c.hook();
    return poll ? poll() : true;
}

void chip_engine_safe_state(void) { c.safe++; }

static uint16_t cancelled(void)
{
    chip_engine_safe_state();
    return PDK_CANCELLED;
}

uint16_t chip_engine_probe(uint16_t *id12, uint8_t *family, chip_poll_fn poll)
{
    if (!poll_now(poll))
        return cancelled();
    chip_engine_safe_state();
    if (!c.chip)
        return PDK_CHIP_NOT_FOUND;
    *id12 = c.id12;
    *family = c.chip->family;
    return PDK_OK;
}

uint16_t chip_engine_read(const struct chip_info *chip, uint16_t addr, uint16_t count, chip_sink_fn sink, void *ctx,
                          chip_poll_fn poll)
{
    if (!c.chip) {
        chip_engine_safe_state();
        return PDK_CHIP_NOT_FOUND;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (i % READ_POLL_EVERY == 0 && !poll_now(poll))
            return cancelled();
        sink(ctx, (uint16_t)(addr + i), (uint16_t)(c.mem[addr + i] & chip_word_mask(chip)));
    }
    chip_engine_safe_state();
    return PDK_OK;
}

uint16_t chip_engine_erase(const struct chip_info *chip, chip_poll_fn poll)
{
    if (!poll_now(poll))
        return cancelled();
    if (!c.chip) {
        chip_engine_safe_state();
        return PDK_CHIP_NOT_FOUND;
    }
    c.erases++;
    for (unsigned i = 0; i < chip->code_words; i++) {
        c.mem[i] = chip_word_mask(chip);
        c.written[i] = 0;
    }
    chip_engine_safe_state();
    return PDK_OK;
}

uint16_t chip_engine_write_block(const struct chip_info *chip, uint16_t addr, const uint16_t *words, uint8_t count,
                                 chip_poll_fn poll)
{
    if (!poll_now(poll))
        return cancelled();
    if (!c.chip) {
        chip_engine_safe_state();
        return PDK_CHIP_NOT_FOUND;
    }
    c.writes++;
    for (unsigned i = 0; i < count; i++) {
        uint16_t a = (uint16_t)(addr + i);
        if (c.weak && a == c.weak_addr && c.written[a])
            continue;
        if (words[i] != chip_word_mask(chip))
            c.written[a]++;
        c.mem[a] &= words[i];
    }
    chip_engine_safe_state();
    return PDK_OK;
}

uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint32_t target_hz,
                               uint16_t mV, uint8_t *value, uint32_t *measured_hz, chip_poll_fn poll)
{
    if (!poll_now(poll))
        return cancelled();
    c.calibrations++;
    chip_engine_safe_state();
    calib_match_t m;
    if (!calib_match(chip, c.mem + site_addr, chip->code_words - site_addr, site_addr, &m) ||
        m.form != CALIB_FORM_LOOP || m.kind != kind) {
        *value = 0;
        *measured_hz = 0;
        return PDK_CALIB_FAILED;   /* no loop there: nothing toggles PA3 */
    }
    fake_cal_t outcome = FAKE_CAL_OK;
    uint8_t v = 0x40;
    if (c.script_pos < c.script_len) {
        outcome = c.script[c.script_pos];
        v = c.script_value[c.script_pos++];
    }
    *value = v;
    *measured_hz = outcome == FAKE_CAL_OK ? target_hz : target_hz / 2u;
    switch (outcome) {
    case FAKE_CAL_FAIL: return PDK_CALIB_FAILED;
    case FAKE_CAL_WRONG_SITE: *measured_hz = target_hz * 100u; return PDK_CALIB_WRONG_SITE;
    default: return PDK_OK;
    }
}

uint16_t chip_engine_measure_freq(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint8_t start_value,
                                  uint16_t mV, uint32_t *hz, chip_poll_fn poll)
{
    if (!poll_now(poll))
        return cancelled();
    chip_engine_safe_state();
    calib_match_t m;
    if (!c.chip || !calib_match(chip, c.mem + site_addr, chip->code_words - site_addr, site_addr, &m) ||
        m.form != CALIB_FORM_LOOP || c.measure_hz == 0) {
        *hz = 0;
        return PDK_CALIB_FAILED;   /* no signal */
    }
    *hz = kind == CALIB_KIND_ILRC ? c.measure_hz / 128u : c.measure_hz;
    return PDK_OK;
}

uint16_t chip_engine_run_start(uint16_t vdd_mV)
{
    chip_engine_safe_state();
    return power_apply(PDK_RAIL_VDD, vdd_mV, POWER_PAUSE_VDD_CMD_US, NULL);
}

void chip_engine_run_stop(void)
{
    hal_power_off_all();
    chip_engine_safe_state();
}
