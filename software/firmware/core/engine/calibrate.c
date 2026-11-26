/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Oscillator and BandGap calibration on the running IC (docs/easy-pdk-programmer-protocol.md §6.3, research R9; ported from
 * the free-pdk firmware, fpdk.c, Copyright (C) 2019-2020 freepdk https://free-pdk.github.io, GPL-3.0-or-later).
 *
 * The IC runs the measurement loop written in place of the stub (VDD only, VPP 0): every pass toggles PA3 and is exactly
 * 7 clocks, so SYSCLK = 7 · f(PA3); a 1 on PA4 during a pass adds one to the trim value. The programmer counts PA3 edges
 * over a window and shifts "+1" pulses with the HAL calibration counter, and searches:
 *   IHRC  — trim 1…0x9F in steps of 1, the frequency closest to the target;
 *   ILRC  — 0x00…0xF0 in steps of 0x10;
 *   BG    — 0x00…0xF0 in steps of 0x10, ≥ 500 µs settling, the first value where the comparator output PA0 reads 0.
 * The FR-055 checks of feature 001 make sure it is the loop of exactly this stub: the first reading within ¼…4 of the
 * target, a frequency that rises with the steps, PA0 driven by the IC (it does not follow the pull-up / pull-down).
 * SNAP_STANDARD (target 0xFF0000kk) sweeps to the master's value kk and calibrates to the nearest standard SYSCLK when it
 * is within 10 %; MEASURE_MASTER measures the loop as it runs, without pulses. */
#include <string.h>

#include "engine_internal.h"
#include "chips/calib_footprint.h"
#include "chips/calib_sites.h"
#include "hal_time.h"
#include "proto/proto_defs.h"

#define LOOP_CLOCKS 7u
#define WINDOW_MIN_US 2000u
#define WINDOW_STEP_MAX_US 100000u   /* one HAL call; longer windows are accumulated between polls */
#define WINDOW_TOTAL_MAX_US 400000u
#define EDGES_WANTED 2000u           /* ≤ 0.05 % per count */
#define SIGNAL_TIMEOUT_US 1000000u   /* no loop running: "no signal" */
#define PULSE_TIMEOUT_US 1000000u
#define BG_SETTLE_NS 500000u
#define IHRC_LAST 0x9Fu
#define RC_STEP_ILRC 0x10u
#define BG_LAST 0xF0u
#define REACH_PERCENT 2u             /* the best value must be within 2 % of the target */
#define RISE_PERCENT 1u              /* over the sweep the frequency must rise by more than measurement noise */

static uint32_t sweep[256];          /* SYSCLK per trim value (SNAP needs the whole curve) */

/* SYSCLK of the running loop: PA3 edges over a window long enough for EDGES_WANTED at `expected_hz`. 0 = no edges. */
static uint16_t measure(uint32_t expected_hz, uint32_t *hz, chip_poll_fn poll)
{
    uint32_t pa3 = expected_hz / LOOP_CLOCKS;
    uint64_t want_us = pa3 ? (uint64_t)EDGES_WANTED * 1000000u / pa3 : WINDOW_TOTAL_MAX_US;
    if (want_us < WINDOW_MIN_US)
        want_us = WINDOW_MIN_US;
    if (want_us > WINDOW_TOTAL_MAX_US)
        want_us = WINDOW_TOTAL_MAX_US;
    uint32_t edges = 0, elapsed = 0;
    while (elapsed < want_us) {
        if (!eng_poll(poll))
            return PDK_CANCELLED;
        uint32_t step = (uint32_t)(want_us - elapsed) > WINDOW_STEP_MAX_US ? WINDOW_STEP_MAX_US : (uint32_t)(want_us - elapsed);
        uint32_t got_us = 0;
        edges += hal_icp_calib_count(step, &got_us);
        elapsed += got_us ? got_us : step;
    }
    *hz = elapsed ? (uint32_t)((uint64_t)edges * LOOP_CLOCKS * 1000000u / elapsed) : 0;
    return PDK_OK;
}

/* Waits until the IC's program reaches the loop (PA3 toggles), at most 1 s. */
static uint16_t wait_signal(uint32_t expected_hz, uint32_t *hz, chip_poll_fn poll)
{
    uint32_t t0 = hal_time_us();
    for (;;) {
        uint16_t st = measure(expected_hz, hz, poll);
        if (st != PDK_OK)
            return st;
        if (*hz)
            return PDK_OK;
        if (hal_time_us() - t0 >= SIGNAL_TIMEOUT_US)
            return PDK_CALIB_FAILED;   /* no signal: the program does not reach this loop */
    }
}

static uint16_t step_trim(uint16_t n)
{
    while (n) {   /* the pulse buffer takes a limited number of bits at a time */
        uint16_t k = n > 64u ? 64u : n;
        if (!hal_icp_calib_pulses(k, PULSE_TIMEOUT_US))
            return PDK_CALIB_FAILED;   /* the loop stopped clocking */
        n = (uint16_t)(n - k);
    }
    return PDK_OK;
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* VDD on for the IC's program, the counter started, the loop found. */
static uint16_t start(const struct chip_info *chip, uint16_t mV, uint32_t expected_hz, uint32_t *f0, chip_poll_fn poll)
{
    uint16_t limit = hal_icp_vdd_max_mV();
    if (limit && mV > limit)
        return eng_fail(PDK_OP_UNSUPPORTED);   /* the IC would drive a programmer pin above its rating (research R10) */
    uint16_t st = eng_supply(chip, MODE_CALIB, mV ? mV : 5000u, poll);
    if (st != PDK_OK)
        return st;
    if (!hal_icp_calib_begin())
        return eng_fail(PDK_OP_UNSUPPORTED);
    st = wait_signal(expected_hz, f0, poll);
    return st == PDK_OK ? PDK_OK : eng_fail(st);
}

static uint16_t finish(uint16_t st)
{
    hal_icp_calib_end();
    return eng_fail(st);   /* VDD off, lines idle */
}

/* The trim sweep from 0 up to `last` in steps of `step`; stops early once the frequency has passed the target and moves away
 * from it (unless `full`). Fills sweep[], returns the best value and the number of points. */
static uint16_t do_sweep(uint32_t target, uint32_t f0, unsigned last, unsigned step, bool full, unsigned *best,
                         unsigned *top, chip_poll_fn poll)
{
    sweep[0] = f0;
    *best = 0;
    *top = 0;
    for (unsigned k = step; k <= last; k += step) {
        uint16_t st = step_trim((uint16_t)step);
        if (st == PDK_OK)
            st = measure(sweep[k - step] ? sweep[k - step] : target, &sweep[k], poll);
        if (st != PDK_OK)
            return st;
        *top = k;
        if (target && distance(sweep[k], target) < distance(sweep[*best], target))
            *best = k;
        if (!full && target && sweep[k] > target && distance(sweep[k], target) > distance(sweep[*best], target))
            break;   /* past the target and moving away: the frequency only rises from here */
    }
    return PDK_OK;
}

/* FR-055 (2): the frequency must respond to the steps. */
static bool rises(unsigned top)
{
    return top && (uint64_t)sweep[top] * 100u > (uint64_t)sweep[0] * (100u + RISE_PERCENT);
}

static uint16_t calibrate_rc(const struct chip_info *chip, uint8_t kind, uint32_t target, uint16_t mV, uint8_t *value,
                             uint32_t *measured_hz, chip_poll_fn poll)
{
    bool snap = calib_is_snap(target);
    unsigned kk = target & 0xFFu;
    uint32_t f0;
    uint16_t st = start(chip, mV, snap ? 8000000u : target, &f0, poll);
    if (st != PDK_OK)
        return st;
    *measured_hz = f0;
    if (!snap && (f0 < target / 4u || f0 > target * 4u))
        return finish(PDK_CALIB_WRONG_SITE);   /* FR-055 (1): not the loop of this stub */
    unsigned step = kind == CALIB_KIND_ILRC ? RC_STEP_ILRC : 1u;
    unsigned last = kind == CALIB_KIND_ILRC ? 0xF0u : (snap && kk > IHRC_LAST ? kk : IHRC_LAST);
    unsigned best, top;
    if ((st = do_sweep(snap ? 0 : target, f0, last, step, snap, &best, &top, poll)) != PDK_OK)
        return finish(st);
    if (!rises(top))
        return finish(PDK_CALIB_WRONG_SITE);   /* FR-055 (2): no response to the steps ("deaf", wrong loop) */
    if (snap) {   /* the nearest standard SYSCLK if the copy at the master's value is within 10 % of it */
        static const uint32_t standard[] = {16000000, 8000000, 4000000, 2000000, 1000000, 500000};
        uint32_t f_kk = sweep[kk], std = standard[0];
        for (unsigned i = 1; i < sizeof standard / sizeof standard[0]; i++)
            if (distance(f_kk, standard[i]) < distance(f_kk, std))
                std = standard[i];
        best = kk;
        if (distance(f_kk, std) <= std / 10u)
            for (unsigned k = 0; k <= top; k++)
                if (distance(sweep[k], std) < distance(sweep[best], std))
                    best = k;
    }
    *value = (uint8_t)best;
    *measured_hz = sweep[best];
    if (!snap && (uint64_t)distance(sweep[best], target) * 100u > (uint64_t)target * REACH_PERCENT)
        return finish(PDK_CALIB_FAILED);   /* the target cannot be reached: the best value is reported */
    return finish(PDK_OK);
}

/* FR-055 (3): the IC drives PA0 (the comparator output) — it does not follow the programmer's pulls. */
static bool pa0_driven(void)
{
    hal_icp_mode(ICP_PA0, ICP_IN_PULLUP);
    hal_icp_delay_ns(20000u);
    bool up = hal_icp_level(ICP_PA0);
    hal_icp_mode(ICP_PA0, ICP_IN_PULLDOWN);
    hal_icp_delay_ns(20000u);
    bool down = hal_icp_level(ICP_PA0);
    hal_icp_mode(ICP_PA0, ICP_IN);
    return !(up && !down);
}

static uint16_t calibrate_bg(const struct chip_info *chip, uint16_t mV, uint8_t *value, chip_poll_fn poll)
{
    uint32_t f0;
    uint16_t st = start(chip, mV, 1000000u, &f0, poll);
    if (st != PDK_OK)
        return st;
    if (!pa0_driven())
        return finish(PDK_CALIB_WRONG_SITE);
    for (unsigned k = 0; k <= BG_LAST; k += RC_STEP_ILRC) {
        if (!eng_poll(poll))
            return finish(PDK_CANCELLED);
        if (k && (st = step_trim(RC_STEP_ILRC)) != PDK_OK)
            return finish(st);
        hal_icp_delay_ns(BG_SETTLE_NS);
        if (!hal_icp_level(ICP_PA0)) {
            *value = (uint8_t)k;
            return finish(PDK_OK);
        }
    }
    *value = BG_LAST;
    return finish(PDK_CALIB_FAILED);   /* the comparator never flipped */
}

uint16_t chip_engine_calibrate(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint32_t target_hz,
                               uint16_t mV, uint8_t *value, uint32_t *measured_hz, chip_poll_fn poll)
{
    (void)site_addr;   /* the IC runs whichever loop its program reaches: FR-055 checks it is the right one */
    *value = 0;
    *measured_hz = 0;
    eng_leave();
    if (kind == CALIB_KIND_BG)
        return calibrate_bg(chip, mV, value, poll);
    if (kind != CALIB_KIND_IHRC && kind != CALIB_KIND_ILRC)
        return eng_fail(PDK_CALIB_FAILED);
    if (calib_is_snap(target_hz) && kind != CALIB_KIND_IHRC)
        return eng_fail(PDK_CALIB_FAILED);
    return calibrate_rc(chip, kind, target_hz, mV, value, measured_hz, poll);
}

uint16_t chip_engine_measure_freq(const struct chip_info *chip, uint16_t site_addr, uint8_t kind, uint8_t start_value,
                                  uint16_t mV, uint32_t *hz, chip_poll_fn poll)
{
    (void)site_addr;
    (void)start_value;   /* the loop was written to start at it */
    *hz = 0;
    eng_leave();
    uint32_t f0;
    uint16_t st = start(chip, mV, kind == CALIB_KIND_ILRC ? 62500u : 8000000u, &f0, poll);
    if (st != PDK_OK)
        return st;
    st = measure(f0, hz, poll);   /* a full window at the frequency found */
    if (st == PDK_OK && !*hz)
        st = PDK_CALIB_FAILED;
    return finish(st);
}
