/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supply_set.h"

#include "hal_time.h"
#include "power.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "supply_cal.h"

#define IDLE_TIMEOUT_MS (5u * 60u * 1000u)   /* voltages are not left on forever */
#define READ_SAMPLES 32u                     /* spread over ~2.5 ms to catch ripple in min/max */
#define FLAG_NO_CHECK 0x01u                  /* SUPPLY_SET: skip the tolerance check (diagnostics) */

static struct {
    bool active;
    uint16_t set[2];   /* by rail, mV; 0 = off */
    uint32_t last_ms;
} s;

void supply_set_init(void)
{
    s.active = false;
    s.set[0] = s.set[1] = 0;
}

bool supply_set_active(void) { return s.active; }

void supply_set_abort(void)
{
    if (!s.active)
        return;
    power_off_all();
    supply_set_init();
}

void supply_set_poll(void)
{
    if (s.active && hal_time_ms() - s.last_ms > IDLE_TIMEOUT_MS)
        supply_set_abort();
}

/* SUPPLY_STATE: set points, averages, then min/max of each rail (contracts/protocol.md) */
static void put_state(uint8_t *rsp, uint16_t *rsp_len)
{
    uint32_t sum[2] = {0, 0};
    uint16_t lo[2] = {0xFFFFu, 0xFFFFu}, hi[2] = {0, 0};
    for (unsigned i = 0; i < READ_SAMPLES; i++) {
        for (int r = 0; r < 2; r++) {
            uint16_t m = power_measure((pdk_rail_t)r);
            sum[r] += m;
            lo[r] = m < lo[r] ? m : lo[r];
            hi[r] = m > hi[r] ? m : hi[r];
        }
        hal_delay_us(POWER_SAMPLE_GAP_US);
    }
    for (int r = 0; r < 2; r++) {
        put16(rsp + 2 * r, s.set[r]);
        put16(rsp + 4 + 2 * r, (uint16_t)((sum[r] + READ_SAMPLES / 2u) / READ_SAMPLES));
        put16(rsp + 8 + 4 * r, lo[r]);
        put16(rsp + 10 + 4 * r, hi[r]);
    }
    *rsp_len = 16;
}

uint16_t supply_set_cmd_set(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    uint16_t limit[2];
    uint8_t flags = len == 5 ? req[4] : 0;
    if ((len != 4 && len != 5) || (flags & ~FLAG_NO_CHECK))
        return PDK_BAD_PAYLOAD;
    hal_power_limits(&limit[PDK_RAIL_VDD], &limit[PDK_RAIL_VPP]);
    if (!limit[PDK_RAIL_VDD] || !limit[PDK_RAIL_VPP])
        return PDK_OP_UNSUPPORTED;
    if (supply_cal_active())
        return PDK_NOT_ALLOWED_NOW;
    uint16_t want[2] = {le16(req), le16(req + 2)};
    for (int r = 0; r < 2; r++) {
        if (want[r] > limit[r]) {
            rsp[0] = (uint8_t)r;
            *rsp_len = 1;
            return PDK_BAD_PAYLOAD;
        }
    }

    /* VPP is lowered before VDD and raised after it */
    pdk_rail_t order[2] = {PDK_RAIL_VDD, PDK_RAIL_VPP};
    if (want[PDK_RAIL_VPP] < s.set[PDK_RAIL_VPP]) {
        order[0] = PDK_RAIL_VPP;
        order[1] = PDK_RAIL_VDD;
    }
    s.active = true;
    s.last_ms = hal_time_ms();
    for (int i = 0; i < 2; i++) {
        pdk_rail_t r = order[i];
        if (flags & FLAG_NO_CHECK) {   /* whatever the rail does, it stays up for the meter */
            power_set_raw(r, want[r]);
            hal_delay_us(POWER_PAUSE_HV_US);
            s.set[r] = want[r];
            continue;
        }
        power_fault_t f;
        uint16_t st = power_apply(r, want[r], POWER_PAUSE_HV_US, &f);
        if (st != PDK_OK) {   /* power_apply() has already switched everything off */
            supply_set_init();
            rsp[0] = f.rail;
            put16(rsp + 1, f.set_mV);
            put16(rsp + 3, f.meas_mV);
            *rsp_len = 5;
            return st;
        }
        s.set[r] = want[r];
    }
    if (!want[PDK_RAIL_VDD] && !want[PDK_RAIL_VPP])
        s.active = false;
    put_state(rsp, rsp_len);
    return PDK_OK;
}

uint16_t supply_set_cmd_get(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    uint16_t vdd_max, vpp_max;
    if (len != 0)
        return PDK_BAD_PAYLOAD;
    hal_power_limits(&vdd_max, &vpp_max);
    if (!vdd_max || !vpp_max)
        return PDK_OP_UNSUPPORTED;
    if (s.active)
        s.last_ms = hal_time_ms();
    put_state(rsp, rsp_len);
    return PDK_OK;
}
