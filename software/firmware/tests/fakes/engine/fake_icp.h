/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fake IC programming lines (hal_icp.h) for the engine tests: every call and every supply change is recorded as an event
 * (the trace of specs/002-chip-engine/contracts/traces.md) and forwarded to the IC model (fake_icp_ic.h). */
#ifndef PDK_FAKE_ICP_H
#define PDK_FAKE_ICP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_icp.h"

typedef enum {
    FEV_VDD = 0,   /* mV */
    FEV_VPP,       /* mV */
    FEV_MODE,      /* line, dir */
    FEV_CLOCK,     /* ns = high time */
    FEV_SEND,      /* line = data, clk, value, bits */
    FEV_RECV,      /* line = data, clk, bits, value = what the model answered */
    FEV_DELAY,     /* ns */
    FEV_IDLE,
    FEV_CAL_BEGIN,
    FEV_CAL_PULSES,   /* value = n */
    FEV_CAL_COUNT,    /* ns = window */
    FEV_CAL_END,
} fake_ev_kind_t;

typedef struct {
    fake_ev_kind_t kind;
    uint8_t line, clk, dir, bits;
    uint32_t value;
    uint32_t ns;
    uint16_t mV;
    uint32_t t_us;   /* fake time when it happened */
} fake_ev_t;

void fake_icp_reset(void);
/* Recording: on by default; the list keeps the first FAKE_ICP_MAX_EVENTS events. */
#define FAKE_ICP_MAX_EVENTS 8192u
void fake_icp_record(bool on);
size_t fake_icp_events(const fake_ev_t **events);
void fake_icp_clear(void);

/* Line levels as the fake sees them (driven by the programmer or by the model). */
bool fake_icp_driven(icp_line_t line, bool *level);

/* Calibration counter: the IC's SYSCLK as a function of its trim value k (0 = no loop running), and the BandGap
 * comparator output on PA0 (flips to 0 at k >= bg_flip; bg_flip < 0: PA0 not driven by the IC). */
void fake_icp_calib_curve(uint32_t (*sysclk)(uint8_t k));
void fake_icp_calib_bandgap(int bg_flip);
uint8_t fake_icp_calib_trim(void);
void fake_icp_vdd_limit(uint16_t mV);

#endif
