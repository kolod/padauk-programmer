/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Programming supplies (contracts/chip-engine.md §1). Only core/power uses these directly. */
#ifndef PDK_HAL_POWER_H
#define PDK_HAL_POWER_H

#include <stdint.h>

typedef enum { PDK_RAIL_VDD = 0, PDK_RAIL_VPP = 1 } pdk_rail_t;

void hal_power_set(pdk_rail_t rail, uint16_t mV);    /* 0 = off with active discharge */
uint16_t hal_power_measure(pdk_rail_t rail);         /* measured voltage, mV */
void hal_power_off_all(void);                        /* VDD = VPP = 0 now; safe from any context */
void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV);   /* 0, 0 = no supply front end */

#endif
