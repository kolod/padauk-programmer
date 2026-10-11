/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supply model: linear ramp to the target in SETTLE_US, small noise, optional faults. */
#include "hal_power.h"
#include "hal_time.h"
#include "host.h"

#include <stdlib.h>
#include <string.h>

#define SETTLE_US 300u
#define SLOW_SETTLE_US 60000u

typedef enum { MODE_OK = 0, MODE_LOW, MODE_SLOW, MODE_STUCK } mode_t_;

static struct {
    uint16_t target, from;
    uint32_t set_at;
    mode_t_ mode;
} rail[2];

bool host_power_fault(const char *spec)
{
    int r;
    if (!strncmp(spec, "vdd:", 4))
        r = PDK_RAIL_VDD;
    else if (!strncmp(spec, "vpp:", 4))
        r = PDK_RAIL_VPP;
    else
        return false;
    const char *m = spec + 4;
    if (!strcmp(m, "low"))
        rail[r].mode = MODE_LOW;
    else if (!strcmp(m, "slow"))
        rail[r].mode = MODE_SLOW;
    else if (!strcmp(m, "stuck"))
        rail[r].mode = MODE_STUCK;
    else
        return false;
    return true;
}

static uint16_t ideal(pdk_rail_t r)
{
    uint32_t settle = rail[r].mode == MODE_SLOW ? SLOW_SETTLE_US : SETTLE_US;
    uint32_t dt = hal_time_us() - rail[r].set_at;
    if (dt >= settle)
        return rail[r].target;
    return (uint16_t)(rail[r].from + ((int32_t)rail[r].target - rail[r].from) * (int32_t)dt / (int32_t)settle);
}

void hal_power_set(pdk_rail_t r, uint16_t mV)
{
    rail[r].from = ideal(r);
    rail[r].target = mV;
    rail[r].set_at = hal_time_us();
}

uint16_t hal_power_measure(pdk_rail_t r)
{
    int v;
    switch (rail[r].mode) {
    case MODE_STUCK: v = rail[r].from; break;
    case MODE_LOW: v = rail[r].target * 9 / 10; break;
    default: v = ideal(r); break;
    }
    v += (rand() % 9) - 4;   /* ±4 mV noise */
    return (uint16_t)(v < 0 ? 0 : v);
}

void hal_power_off_all(void)
{
    for (int r = 0; r < 2; r++) {
        rail[r].from = ideal((pdk_rail_t)r);
        rail[r].target = 0;
        rail[r].set_at = hal_time_us();
    }
}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = 6260;
    *vpp_max_mV = 13270;
}
