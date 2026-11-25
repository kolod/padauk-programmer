/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Operation manager (data-model Operation): one operation at a time, IDLE → RUNNING → DONE, op_id,
 * stages and progress (EVT_PROGRESS on every stage change and at least every 500 ms), cancellation,
 * RESULT. A started operation runs to the end from pdk_core_poll(); while it runs, the engine's poll()
 * keeps serving the session, so commands allowed during an operation are answered and the rest get
 * BUSY. Losing the session does not stop an operation (FR-023). */
#ifndef PDK_OP_MANAGER_H
#define PDK_OP_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/calib_sites.h"

/* OP_START op_flags (PROGRAM only; OR-ed onto the package flags, only towards "skip") */
enum {
    PDK_OPF_SKIP_ERASE = 1u << 0,
    PDK_OPF_SKIP_BLANK_CHECK = 1u << 1,
    PDK_OPF_SKIP_VERIFY = 1u << 2,
    PDK_OPF_CALIB_FINALIZE_ON_FAIL = 1u << 4,
};
#define PDK_OPF_ALLOWED (PDK_OPF_SKIP_ERASE | PDK_OPF_SKIP_BLANK_CHECK | PDK_OPF_SKIP_VERIFY | \
                         PDK_OPF_CALIB_FINALIZE_ON_FAIL)

enum { PDK_SOURCE_HOST = 0, PDK_SOURCE_LOCAL = 1 };

#define OP_PROGRESS_INTERVAL_MS 500u
#define OP_MAX_LEVELS 16u

typedef struct {
    uint8_t kind, value;
    uint32_t target_hz, measured_hz;
    uint8_t mode, master_value, site_status;
} op_calib_result_t;

typedef struct {
    uint8_t rail;
    uint16_t set_mV, meas_mV, settle_us;
    uint8_t ok;
} op_level_t;

/* RESULT (contracts/protocol.md §4) */
typedef struct {
    uint16_t op_id;
    uint8_t op_type, source;
    uint16_t status;
    uint8_t stage;
    uint16_t chip_id_found;
    uint8_t family_found;
    uint16_t fail_addr, expected, actual;
    uint8_t serial_valid;
    uint8_t serial[8];   /* little-endian u64 */
    uint16_t fuse_written;
    uint32_t data_words, data_crc32;
    uint32_t elapsed_ms;
    uint8_t calib_count;
    op_calib_result_t calib[CALIB_MAX_SITES];
    uint32_t image_crc_raw, image_crc_norm;
    uint8_t warnings;
    uint8_t v_rail;
    uint16_t v_set_mV, v_meas_mV;
    uint8_t level_count;
    op_level_t levels[OP_MAX_LEVELS];
} op_result_t;

#define OP_EXT_MAX (7u + 64u * 4u)   /* the longest ext: PATCH with 64 words */

typedef struct {
    uint8_t op_type, op_flags, source;
    uint16_t chip_id12, param;
    uint16_t ext_len;
    uint8_t ext[OP_EXT_MAX];
} op_request_t;

void op_manager_init(void);

/* Queues an operation; it starts on the next op_manager_poll(). PDK_OK and *op_id, PDK_BUSY while one
 * runs or a package upload is in progress, PDK_OP_UNSUPPORTED for a type not implemented, PDK_BAD_PAYLOAD
 * for op_flags outside PROGRAM, PDK_PKG_CHIP_UNSUPPORTED for an unknown chip_id12. */
uint16_t op_manager_start(const op_request_t *req, uint16_t *op_id);

/* Runs a queued operation to the end (call from the main loop). */
void op_manager_poll(void);

/* True while an operation is queued or running. */
bool op_manager_busy(void);

/* Requests cancellation of the running operation; false if `op_id` is not the running one. */
bool op_manager_cancel(uint16_t op_id);

/* RESULT of `op_id` (0 = the last one); NULL if unknown or still running. */
const op_result_t *op_manager_result(uint16_t op_id);

/* RESULT encoding; returns the length. */
uint16_t op_result_encode(const op_result_t *r, uint8_t *out);

/* ---- for the operation pipelines ---- */

/* Declares the stages the operation will go through (for stage_index / stage_count). */
void op_plan(const uint8_t *stages, uint8_t count);
/* Enters `stage` (sends EVT_PROGRESS); a stage not in the plan is appended. */
void op_stage(uint8_t stage);
/* Progress inside the current stage, 0…100. */
void op_percent(uint8_t percent);
/* The engine's poll callback: serves the session, sends due progress; false = cancel requested. */
bool op_poll(void);
op_result_t *op_current(void);
const op_request_t *op_request(void);

/* For STATUS: the running or last operation (op_id 0 = none yet) and its progress. */
typedef struct {
    bool running;
    uint16_t op_id;
    uint8_t op_type, stage, percent_total;
} op_status_t;
void op_manager_status(op_status_t *s);

/* Pipelines, one per operation type (op_program.c, op_simple.c): fill op_current() and return the status. */
uint16_t op_program_run(void);
uint16_t op_probe_run(void);
uint16_t op_erase_run(void);
uint16_t op_blank_check_run(void);
uint16_t op_verify_run(void);
uint16_t op_patch_run(void);

#endif
