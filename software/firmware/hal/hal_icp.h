/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PADAUK in-circuit programming lines and the calibration counter (specs/002-chip-engine/contracts/hal-icp.md).
 * Implemented by each MCU platform and by the test fake; used only by the chip engine (core/engine). Supplies are not part
 * of this interface: the engine sets them through core/power (power_apply).
 *
 * Rules:
 *  - every duration is a MINIMUM: a level may be longer (an interrupt, a slow edge), never shorter;
 *  - no function blocks for more than 100 ms;
 *  - hal_icp_idle() leaves no line driven and is safe to call at any time;
 *  - the HAL never drives the clock line while the calibration counter is active. */
#ifndef PDK_HAL_ICP_H
#define PDK_HAL_ICP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ICP_CLK = 0,   /* IC PA3: programming clock; IC output during calibration */
    ICP_DAT,       /* IC PA6: FLASH data both ways, OTP data from the IC */
    ICP_PA4,       /* IC PA4: OTP data into the IC; "+1" pulses during calibration */
    ICP_PA0,       /* IC PA0: OTP3_1 second clock; BandGap comparator output */
    ICP_PA7,       /* IC PA7: OTP3_1 "commit" */
    ICP_LINES
} icp_line_t;

typedef enum {
    ICP_OUT_LOW = 0,
    ICP_OUT_HIGH,
    ICP_IN,
    ICP_IN_PULLDOWN,
    ICP_IN_PULLUP,     /* with ICP_IN_PULLDOWN: tells a line the IC drives from a floating one (BandGap PA0) */
} icp_dir_t;

/* All lines inputs, CLK and DAT with pull-down; stops the calibration peripherals. */
void hal_icp_idle(void);

/* Direction / level of one line. */
void hal_icp_mode(icp_line_t line, icp_dir_t dir);

/* One pulse on CLK: high for at least high_ns, then low. */
void hal_icp_clock(uint32_t high_ns);

/* MSB first: for each bit set `data`, `clk` high for at least 1 µs, `clk` low. bits <= 32. */
void hal_icp_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits);

/* MSB first: `clk` high, at least 1 µs, sample `data`, `clk` low. Returns the last <= 32 bits. */
uint32_t hal_icp_recv(icp_line_t data, icp_line_t clk, uint8_t bits);

/* Level of one line (as an input or the driven level). */
bool hal_icp_level(icp_line_t line);

/* Busy wait of at least `ns` nanoseconds (cycle counter). */
void hal_icp_delay_ns(uint32_t ns);

/* The highest IC VDD at which the IC may drive the lines the programmer reads (0 = no limit beyond the supply). */
uint16_t hal_icp_vdd_max_mV(void);

/* ---- calibration counter ---- */

/* CLK becomes the IC's output into the counter, PA4 the "+1" output (low); the counter is reset. */
bool hal_icp_calib_begin(void);

/* Emits exactly n one-bits on PA4, one per IC loop turn (synchronous with CLK edges); false if the IC did not clock
 * them out within timeout_us (no loop running). */
bool hal_icp_calib_pulses(uint16_t n, uint32_t timeout_us);

/* Counts CLK rising edges for at least window_us (<= 100 ms); returns the count and the measured window. *ref_edges: the
 * same edges counted by an independent path (on the G474 the bits the pulse output shifted, from its own pin), a
 * cross-check for spurious edges in the diagnostic trace; 0 if the platform has none. */
uint32_t hal_icp_calib_count(uint32_t window_us, uint32_t *elapsed_us, uint32_t *ref_edges);

/* Stops the counter and the pulse output; lines back to idle. */
void hal_icp_calib_end(void);

#endif
