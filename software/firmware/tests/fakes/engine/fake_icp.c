/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fake_icp.h"

#include <string.h>

#include "fake_hal.h"
#include "fake_icp_ic.h"
#include "hal_power.h"

static struct {
    fake_ev_t ev[FAKE_ICP_MAX_EVENTS];
    size_t n;
    bool record;
    icp_dir_t dir[ICP_LINES];
    uint32_t (*curve)(uint8_t k);
    int bg_flip;
    bool calib;
    uint8_t trim;
    uint16_t vdd_limit;
    uint32_t ns_frac;   /* sub-microsecond remainder of the delays */
} f;

static void advance_ns(uint32_t ns)
{
    f.ns_frac += ns;
    fake_advance_us(f.ns_frac / 1000u);
    f.ns_frac %= 1000u;
    if (f.ns_frac) {   /* round up: a delay is never shorter than asked */
        fake_advance_us(1);
        f.ns_frac = 0;
    }
}

static fake_ev_t *add(fake_ev_kind_t kind)
{
    static fake_ev_t spare;
    if (!f.record || f.n >= FAKE_ICP_MAX_EVENTS)
        return &spare;
    fake_ev_t *e = &f.ev[f.n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->t_us = fake_now_us();
    return e;
}

static void on_power(pdk_rail_t rail, uint16_t mV)
{
    fake_ev_t *e = add(rail == PDK_RAIL_VPP ? FEV_VPP : FEV_VDD);
    e->mV = mV;
    fake_ic_power(rail == PDK_RAIL_VPP, mV, fake_now_us());
}

void fake_engine_reset(void) { fake_icp_reset(); }

void fake_icp_reset(void)
{
    memset(&f, 0, sizeof f);
    f.record = true;
    f.bg_flip = -1;
    for (unsigned i = 0; i < ICP_LINES; i++)
        f.dir[i] = ICP_IN;
    fake_ic_reset();
    fake_power_hook(on_power);
}

void fake_icp_record(bool on) { f.record = on; }
void fake_icp_clear(void) { f.n = 0; }

size_t fake_icp_events(const fake_ev_t **events)
{
    *events = f.ev;
    return f.n;
}

bool fake_icp_driven(icp_line_t line, bool *level)
{
    bool out = f.dir[line] == ICP_OUT_LOW || f.dir[line] == ICP_OUT_HIGH;
    if (level)
        *level = f.dir[line] == ICP_OUT_HIGH;
    return out;
}

void fake_icp_calib_curve(uint32_t (*sysclk)(uint8_t k)) { f.curve = sysclk; }
void fake_icp_calib_bandgap(int bg_flip) { f.bg_flip = bg_flip; }
uint8_t fake_icp_calib_trim(void) { return f.trim; }
void fake_icp_vdd_limit(uint16_t mV) { f.vdd_limit = mV; }

/* ---------------------------------------------------------------- hal_icp.h */

void hal_icp_idle(void)
{
    add(FEV_IDLE);
    f.calib = false;
    for (unsigned i = 0; i < ICP_LINES; i++) {
        bool was_high = f.dir[i] == ICP_OUT_HIGH;
        f.dir[i] = (i == ICP_CLK || i == ICP_DAT) ? ICP_IN_PULLDOWN : ICP_IN;
        if (was_high)
            fake_ic_line((icp_line_t)i, false, fake_now_us());
    }
}

void hal_icp_mode(icp_line_t line, icp_dir_t dir)
{
    fake_ev_t *e = add(FEV_MODE);
    e->line = (uint8_t)line;
    e->dir = (uint8_t)dir;
    bool was_high = f.dir[line] == ICP_OUT_HIGH;
    f.dir[line] = dir;
    bool high = dir == ICP_OUT_HIGH;
    if (high != was_high)
        fake_ic_line(line, high, fake_now_us());
}

void hal_icp_clock(uint32_t high_ns)
{
    fake_ev_t *e = add(FEV_CLOCK);
    e->ns = high_ns;
    fake_ic_line(ICP_CLK, true, fake_now_us());
    advance_ns(high_ns);
    fake_ic_line(ICP_CLK, false, fake_now_us());
    advance_ns(100);
}

void hal_icp_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits)
{
    fake_ev_t *e = add(FEV_SEND);
    e->line = (uint8_t)data;
    e->clk = (uint8_t)clk;
    e->value = value;
    e->bits = bits;
    f.dir[data] = (value & 1u) ? ICP_OUT_HIGH : ICP_OUT_LOW;   /* keeps the last bit */
    fake_ic_send(data, clk, value, bits);
    advance_ns(2000u * bits);
}

uint32_t hal_icp_recv(icp_line_t data, icp_line_t clk, uint8_t bits)
{
    uint32_t v = fake_ic_recv(data, clk, bits);
    fake_ev_t *e = add(FEV_RECV);
    e->line = (uint8_t)data;
    e->clk = (uint8_t)clk;
    e->bits = bits;
    e->value = v;
    advance_ns(2000u * bits);
    return v;
}

bool hal_icp_level(icp_line_t line)
{
    bool level;
    if (fake_icp_driven(line, &level))
        return level;
    if (line == ICP_PA0 && f.calib && f.bg_flip >= 0)
        return f.trim < f.bg_flip;   /* the comparator output */
    return f.dir[line] == ICP_IN_PULLUP;
}

void hal_icp_delay_ns(uint32_t ns)
{
    fake_ev_t *e = add(FEV_DELAY);
    e->ns = ns;
    advance_ns(ns);
}

uint16_t hal_icp_vdd_max_mV(void) { return f.vdd_limit; }

bool hal_icp_calib_begin(void)
{
    add(FEV_CAL_BEGIN);
    f.calib = true;
    f.trim = 0;
    return true;
}

bool hal_icp_calib_pulses(uint16_t n, uint32_t timeout_us)
{
    fake_ev_t *e = add(FEV_CAL_PULSES);
    e->value = n;
    if (!f.calib || !f.curve || !f.curve(f.trim)) {
        fake_advance_us(timeout_us);
        return false;
    }
    uint32_t turn_ns = (uint32_t)(7000000000ull / f.curve(f.trim));   /* one loop turn = 7 SYSCLK */
    advance_ns(turn_ns * n);
    f.trim = (uint8_t)(f.trim + n);
    return true;
}

uint32_t hal_icp_calib_count(uint32_t window_us, uint32_t *elapsed_us)
{
    fake_ev_t *e = add(FEV_CAL_COUNT);
    e->ns = window_us * 1000u;
    fake_advance_us(window_us);
    *elapsed_us = window_us;
    if (!f.calib || !f.curve)
        return 0;
    return (uint32_t)((uint64_t)f.curve(f.trim) * window_us / 7000000u);
}

void hal_icp_calib_end(void)
{
    add(FEV_CAL_END);
    f.calib = false;
}
