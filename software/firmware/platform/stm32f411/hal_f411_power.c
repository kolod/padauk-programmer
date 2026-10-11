/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Supplies of the F411 platform: the VDD/VPP front end is not designed yet (the board has no DAC
 * or regulators). Nothing is ever driven; limits are 0 V, measurements read 0 V, so any nonzero
 * set point times out in power_apply() with VDD_FAULT/VPP_FAULT and INFO hides MEASURE_POWER. */
#include "f411.h"
#include "hal_power.h"

void hal_power_set(pdk_rail_t rail, uint16_t mV)
{
    (void)rail;
    (void)mV;
}

uint16_t hal_power_measure(pdk_rail_t rail)
{
    (void)rail;
    return 0;
}

void hal_power_off_all(void) {}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = 0;
    *vpp_max_mV = 0;
}

/* No IC lines are assigned yet; nothing to release. */
void platform_ic_lines_hiz(void) {}
