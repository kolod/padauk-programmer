/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chip model for pdk-sim: the chip engine on a chip in RAM (contracts/chip-engine.md §3).
 *
 *  - ID from --chip; OTP and FLASH semantics (writes only clear bits, FLASH erases), erase counter;
 *  - voltages through power_apply() only (guarantee 2), kept while one step follows another, removed by
 *    safe_state(); probe at VPP 4.5 V / VDD 2.0 V;
 *  - calibration: the chip runs its placeholders in address order (order=reverse: the last one first),
 *    the running measurement loop gives SYSCLK(k) = base · (1 + offset) · (0.75 + k / 512), base = the
 *    target for the kind being calibrated, else the kind's nominal clock; osc=broken never reaches the
 *    target; deaf ignores the "+1" pulses; BandGap flips the comparator at k = 128 − 4 · offset;
 *  - FR-055 checks of the calibrate primitive: first reading within 1/4 … 4 of the target, every step
 *    changes the frequency, PA0 driven for BandGap (pa0=float: not) → CALIB_WRONG_SITE;
 *  - protected: the fuse reads with the protection bit on (research R10);
 *  - weak=ADDR: writes to ADDR after the first one do not stick (FR-048); slow=US: each write block
 *    takes US microseconds (to act in the middle of an operation);
 *  - --chip-state FILE keeps the chip memory across runs; --power-fail-after also counts chip writes
 *    (the cut write is half done). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip_engine.h"
#include "chips/calib_footprint.h"
#include "chips/chip_table.h"
#include "host.h"
#include "platform_common.h"
#include "power/power.h"
#include "proto/proto_defs.h"

#define MAX_WORDS 0x1000u
#define PROBE_VPP_MV 4500u
#define PROBE_VDD_MV 2000u
#define CALIB_DEFAULT_MV 5000u
#define IHRC_NOMINAL_HZ 8000000u
#define ILRC_NOMINAL_HZ 62500u
#define POLL_EVERY 16u

static struct {
    const chip_info_t *chip;   /* NULL: empty socket */
    int offset_pct;
    bool osc_broken, deaf, reverse, pa0_float, protect;
    bool weak;
    uint16_t weak_addr;
    uint32_t slow_us;
    uint16_t mem[MAX_WORDS];
    uint8_t written[MAX_WORDS];
    uint32_t erases;
    const char *state_path;
    uint16_t vdd, vpp;         /* applied set points */
} c;

/* ---------------------------------------------------------------- configuration and state file */

static void blank(void)
{
    uint16_t mask = c.chip ? chip_word_mask(c.chip) : 0xFFFF;
    for (unsigned i = 0; i < MAX_WORDS; i++)
        c.mem[i] = mask;
    memset(c.written, 0, sizeof c.written);
}

bool host_chip_config(const char *spec)
{
    char name[32];
    const char *colon = strchr(spec, ':');
    size_t n = colon ? (size_t)(colon - spec) : strlen(spec);
    if (n >= sizeof name)
        return false;
    memcpy(name, spec, n);
    name[n] = 0;
    const char *state_path = c.state_path;
    memset(&c, 0, sizeof c);   /* a later --chip replaces every option of an earlier one */
    c.state_path = state_path;
    if (strcmp(name, "none") != 0) {
        for (size_t i = 0; i < chip_count() && !c.chip; i++)
            if (chip_name_matches(chip_at(i), name))
                c.chip = chip_at(i);
        if (!c.chip)
            return false;
    }
    for (const char *o = colon; o && *o;) {
        o++;
        const char *end = strchr(o, ',');
        size_t len = end ? (size_t)(end - o) : strlen(o);
        if (!strncmp(o, "offset=", 7))
            c.offset_pct = atoi(o + 7);
        else if (len == 10 && !strncmp(o, "osc=broken", 10))
            c.osc_broken = true;
        else if (!strncmp(o, "weak=", 5)) {
            c.weak = true;
            c.weak_addr = (uint16_t)strtoul(o + 5, NULL, 0);
        } else if (len == 13 && !strncmp(o, "order=reverse", 13))
            c.reverse = true;
        else if (len == 4 && !strncmp(o, "deaf", 4))
            c.deaf = true;
        else if (len == 9 && !strncmp(o, "pa0=float", 9))
            c.pa0_float = true;
        else if (len == 9 && !strncmp(o, "protected", 9))
            c.protect = true;
        else if (!strncmp(o, "slow=", 5))
            c.slow_us = (uint32_t)strtoul(o + 5, NULL, 0);
        else
            return false;
        o = end;
    }
    blank();
    return true;
}

static void save(void)
{
    if (!c.state_path || !c.chip)
        return;
    FILE *f = fopen(c.state_path, "wb");
    if (!f)
        return;
    fwrite(c.mem, 2, c.chip->code_words, f);
    fclose(f);
}

bool host_chip_state(const char *path)
{
    c.state_path = path;
    if (!c.chip)
        return true;
    FILE *f = fopen(path, "rb");
    if (!f)
        return true;   /* a new chip */
    size_t got = fread(c.mem, 2, c.chip->code_words, f);
    fclose(f);
    if (got != c.chip->code_words)
        blank();
    for (unsigned i = 0; i < c.chip->code_words; i++)
        c.written[i] = c.mem[i] != chip_word_mask(c.chip);
    return true;
}

/* ---------------------------------------------------------------- supplies */

void chip_engine_safe_state(void)
{
    power_off_all();
    platform_ic_lines_hiz();
    host_flash_chip_writing(false);
    c.vdd = c.vpp = 0;
}

/* VPP first, then VDD (entry into the programming mode); nothing is sent before both settled. */
static uint16_t supplies(uint16_t vpp, uint16_t vdd)
{
    uint16_t st = PDK_OK;
    if (vpp != c.vpp)
        st = power_apply(PDK_RAIL_VPP, vpp, POWER_PAUSE_VPP_CMD_US, NULL);
    if (st == PDK_OK && vdd != c.vdd)
        st = power_apply(PDK_RAIL_VDD, vdd, POWER_PAUSE_VDD_CMD_US, NULL);
    if (st != PDK_OK) {   /* power_apply() switched everything off */
        chip_engine_safe_state();
        return st;
    }
    c.vpp = vpp;
    c.vdd = vdd;
    return PDK_OK;
}

static uint16_t fail(uint16_t st)
{
    chip_engine_safe_state();
    return st;
}

/* Guarantee 1: the ID is checked before any high voltage. */
static uint16_t present(const chip_info_t *chip)
{
    if (!c.chip)
        return PDK_CHIP_NOT_FOUND;
    return chip->id12 == c.chip->id12 ? PDK_OK : PDK_CHIP_ID_MISMATCH;
}

/* ---------------------------------------------------------------- primitives */

uint16_t chip_engine_probe(uint16_t *id12, uint8_t *family, chip_poll_fn poll)
{
    if (poll && !poll())
        return fail(PDK_CANCELLED);
    uint16_t st = supplies(PROBE_VPP_MV, PROBE_VDD_MV);
    if (st != PDK_OK)
        return st;
    chip_engine_safe_state();
    if (!c.chip)
        return PDK_CHIP_NOT_FOUND;
    *id12 = c.chip->id12;
    *family = c.chip->family;
    return PDK_OK;
}

uint16_t chip_engine_read(const struct chip_info *chip, uint16_t addr, uint16_t count, chip_sink_fn sink, void *ctx,
                          chip_poll_fn poll)
{
    uint16_t st = present(chip);
    if (st != PDK_OK)
        return fail(st);
    if ((st = supplies(chip->vpp_read, chip->vdd_read)) != PDK_OK)
        return st;
    for (uint32_t i = 0; i < count; i++) {
        if (i % 64u == 0 && poll && !poll())
            return fail(PDK_CANCELLED);
        uint16_t a = (uint16_t)(addr + i), w = (uint16_t)(c.mem[a] & chip_word_mask(chip));
        if (c.protect && a == chip_fuse_addr(chip))
            w &= (uint16_t)~1u;   /* read protection on (what such a chip really returns: research R10) */
        sink(ctx, a, w);
    }
    return PDK_OK;
}

uint16_t chip_engine_erase(const struct chip_info *chip, chip_poll_fn poll)
{
    uint16_t st = present(chip);
    if (st != PDK_OK)
        return fail(st);
    if (!chip_is_flash(chip))
        return fail(PDK_OP_UNSUPPORTED);
    if (poll && !poll())
        return fail(PDK_CANCELLED);
    if ((st = supplies(chip->vpp_erase, chip->vdd_erase)) != PDK_OK)
        return st;
    bool cut = host_power_fail_now();
    c.erases++;
    for (unsigned i = 0; i < chip->code_words; i++)
        if (!cut || i < chip->code_words / 2u) {
            c.mem[i] = chip_word_mask(chip);
            c.written[i] = 0;
        }
    save();
    if (cut)
        host_power_die();
    return PDK_OK;
}

uint16_t chip_engine_write_block(const struct chip_info *chip, uint16_t addr, const uint16_t *words, uint8_t count,
                                 chip_poll_fn poll)
{
    uint16_t st = present(chip);
    if (st != PDK_OK)
        return fail(st);
    if (poll && !poll())
        return fail(PDK_CANCELLED);
    if ((st = supplies(chip->vpp_write, chip->vdd_write)) != PDK_OK)
        return st;
    host_flash_chip_writing(true);
    if (c.slow_us)
        host_sleep_us(c.slow_us);
    bool cut = host_power_fail_now();
    unsigned n = cut ? (count + 1u) / 2u : count;
    for (unsigned i = 0; i < n; i++) {
        uint16_t a = (uint16_t)(addr + i);
        if (c.weak && a == c.weak_addr && c.written[a])
            continue;
        if (words[i] != chip_word_mask(chip))
            c.written[a] = 1;
        c.mem[a] &= words[i];
    }
    save();
    if (cut)
        host_power_die();
    return PDK_OK;
}

/* ---------------------------------------------------------------- calibration */

static uint32_t nominal(uint8_t kind) { return kind == CALIB_KIND_ILRC ? ILRC_NOMINAL_HZ : IHRC_NOMINAL_HZ; }

/* SYSCLK of the running loop with trim value k. */
static uint32_t sysclk(uint8_t run_kind, uint8_t kind, uint32_t target_hz, unsigned k)
{
    double base = run_kind == kind ? (double)target_hz : (double)nominal(run_kind);
    double f = base * (1.0 + c.offset_pct / 100.0) * (0.75 + k / 512.0);
    if (c.osc_broken)
        f *= 0.4;
    return (uint32_t)(f + 0.5);
}

/* The measurement loop the chip actually runs: the first one in address order (order=reverse: last). */
static bool running_loop(uint16_t *addr, calib_match_t *m)
{
    bool found = false;
    for (uint32_t a = 0; a + CALIB_RC_LEN <= c.chip->code_words; a++) {
        calib_match_t t;
        if (calib_match(c.chip, c.mem + a, c.chip->code_words - a, (uint16_t)a, &t) && t.form == CALIB_FORM_LOOP) {
            *addr = (uint16_t)a;
            *m = t;
            found = true;
            if (!c.reverse)
                break;
            a += t.len - 1u;
        }
    }
    return found;
}

uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint32_t target_hz,
                               uint16_t mV, uint8_t *value, uint32_t *measured_hz, chip_poll_fn poll)
{
    *value = 0;
    *measured_hz = 0;
    uint16_t st = present(chip);
    if (st != PDK_OK)
        return fail(st);
    if ((st = supplies(0, mV ? mV : CALIB_DEFAULT_MV)) != PDK_OK)
        return st;
    uint16_t run_addr;
    calib_match_t run;
    if (!running_loop(&run_addr, &run))
        return fail(PDK_CALIB_FAILED);   /* no loop: nothing toggles PA3 */

    if (kind == CALIB_KIND_BG) {
        /* PA0 must follow the comparator, not the weak pull-up/down the engine toggles */
        if (c.pa0_float || run.kind != CALIB_KIND_BG)
            return fail(PDK_CALIB_WRONG_SITE);
        int flip = 128 - 4 * c.offset_pct;
        if (flip < 0 || flip > 255)
            return fail(PDK_CALIB_FAILED);
        for (int k = 0; k <= flip; k++)
            if (k % POLL_EVERY == 0 && poll && !poll())
                return fail(PDK_CANCELLED);
        *value = (uint8_t)flip;
        chip_engine_safe_state();
        return PDK_OK;
    }

    uint32_t f_prev = sysclk(run.kind, kind, target_hz, 0);
    *measured_hz = f_prev;
    if (run.kind == CALIB_KIND_BG || f_prev < target_hz / 4u || f_prev > target_hz * 4u)
        return fail(PDK_CALIB_WRONG_SITE);   /* not the placeholder being calibrated */
    unsigned k = 0;
    while (f_prev < target_hz) {
        if (k == 255) {   /* the target cannot be reached: the best value is the last one */
            *value = 255;
            return fail(PDK_CALIB_FAILED);
        }
        if (k % POLL_EVERY == 0 && poll && !poll())
            return fail(PDK_CANCELLED);
        unsigned next = c.deaf ? k : k + 1;   /* a "+1" pulse on PA4 */
        uint32_t f = sysclk(run.kind, kind, target_hz, next);
        if (f <= f_prev)
            return fail(PDK_CALIB_WRONG_SITE);   /* no response to the step */
        k = next;
        f_prev = f;
    }
    uint32_t below = k ? sysclk(run.kind, kind, target_hz, k - 1u) : 0;
    if (k && target_hz - below < f_prev - target_hz) {
        k--;
        f_prev = below;
    }
    *value = (uint8_t)k;
    *measured_hz = f_prev;
    chip_engine_safe_state();
    return PDK_OK;
}

uint16_t chip_engine_measure_freq(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint8_t start_value,
                                  uint16_t mV, uint32_t *hz, chip_poll_fn poll)
{
    *hz = 0;
    uint16_t st = present(chip);
    if (st != PDK_OK)
        return fail(st);
    if ((st = supplies(0, mV ? mV : CALIB_DEFAULT_MV)) != PDK_OK)
        return st;
    if (poll && !poll())
        return fail(PDK_CANCELLED);
    uint16_t run_addr;
    calib_match_t run;
    if (!running_loop(&run_addr, &run) || run_addr != site_addr || run.kind == CALIB_KIND_BG)
        return fail(PDK_CALIB_FAILED);   /* nothing toggles PA3 at this loop */
    /* the loop starts with ADD A,1: it runs at its immediate + 1; the master's own oscillator, no target */
    *hz = sysclk(run.kind, (uint8_t)~run.kind, 0, (uint8_t)(run.k + 1u));
    chip_engine_safe_state();
    return PDK_OK;
}

uint16_t chip_engine_run_start(uint16_t vdd_mV)
{
    chip_engine_safe_state();
    return PDK_OP_UNSUPPORTED;
}

void chip_engine_run_stop(void) { chip_engine_safe_state(); }
