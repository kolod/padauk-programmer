/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A PADAUK IC behind the fake programming lines, at the granularity of the HAL calls: it decodes the command preamble,
 * answers the FLASH ACK / OTP GetID bits, serves read words, latches write blocks on the programming clocks (FLASH) or
 * the PA4 pulses with the clock held (OTP) — only 1 → 0 — and erases on the erase clocks (FLASH), all only at the
 * operation voltages of its chip. */
#ifndef PDK_FAKE_ICP_IC_H
#define PDK_FAKE_ICP_IC_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"
#include "hal_icp.h"

void fake_ic_reset(void);
void fake_ic_insert(const chip_info_t *chip);   /* a blank chip of this type */
void fake_ic_remove(void);
void fake_ic_set_id(uint16_t id12);             /* answers with another ID */
uint16_t *fake_ic_mem(void);
void fake_ic_weak(uint16_t addr);               /* writes after the first one do not stick */
uint32_t fake_ic_enters(void);                  /* times programming mode was entered */
uint32_t fake_ic_blocks(void);                  /* write blocks latched */
uint32_t fake_ic_erases(void);
uint16_t fake_ic_max_vpp(void);                 /* highest VPP / VDD seen since reset */
uint16_t fake_ic_max_vdd(void);

/* Called by fake_icp.c. */
void fake_ic_power(bool vpp, uint16_t mV, uint32_t t_us);
void fake_ic_line(icp_line_t line, bool high, uint32_t t_us);
void fake_ic_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits);
uint32_t fake_ic_recv(icp_line_t data, icp_line_t clk, uint8_t bits);

#endif
