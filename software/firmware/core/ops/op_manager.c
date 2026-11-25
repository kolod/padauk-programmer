/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "op_manager.h"

#include <string.h>

#include "chip_engine.h"
#include "chips/chip_table.h"
#include "hal_time.h"
#include "hal_ui.h"
#include "indication.h"
#include "package/upload.h"
#include "pdk_config.h"
#include "power/power.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "proto/session.h"
#include "storage/housekeeping.h"
#include "storage/package_store.h"

#define MAX_STAGES 16u

typedef enum { OPS_IDLE = 0, OPS_QUEUED, OPS_RUNNING, OPS_DONE } op_state_t;

static struct {
    op_state_t state;
    uint16_t last_id;
    op_request_t req;
    op_result_t res;
    bool cancel;
    uint8_t plan[MAX_STAGES], plan_n, index;
    uint8_t percent;
    uint32_t start_ms, progress_ms;
} m;

void op_manager_init(void) { memset(&m, 0, sizeof m); }

bool op_manager_busy(void) { return m.state == OPS_QUEUED || m.state == OPS_RUNNING; }

uint16_t op_manager_start(const op_request_t *req, uint16_t *op_id)
{
    if (op_manager_busy() || upload_active())
        return PDK_BUSY;
    switch (req->op_type) {
    case PDK_OP_PROBE:
    case PDK_OP_ERASE:
    case PDK_OP_BLANK_CHECK:
    case PDK_OP_PROGRAM:
    case PDK_OP_VERIFY:
    case PDK_OP_SELF_TEST: break;
#ifdef PDK_FEATURE_PATCH
    case PDK_OP_PATCH:
        if (req->source != PDK_SOURCE_HOST || !req->chip_id12)   /* PC only, chip type required */
            return PDK_BAD_PAYLOAD;
        break;
#endif
    default: return PDK_OP_UNSUPPORTED;
    }
    if ((req->op_flags & ~PDK_OPF_ALLOWED) || (req->op_flags && req->op_type != PDK_OP_PROGRAM))
        return PDK_BAD_PAYLOAD;
    if (req->chip_id12 && !chip_by_id(req->chip_id12))
        return PDK_PKG_CHIP_UNSUPPORTED;
    m.req = *req;
    if (++m.last_id == 0)
        m.last_id = 1;
    *op_id = m.last_id;
    m.state = OPS_QUEUED;
    return PDK_OK;
}

bool op_manager_cancel(uint16_t op_id)
{
    if (!op_manager_busy() || op_id != m.res.op_id)
        return false;
    m.cancel = true;
    return true;
}

const op_result_t *op_manager_result(uint16_t op_id)
{
    if (m.state != OPS_DONE || (op_id && op_id != m.res.op_id))
        return NULL;
    return &m.res;
}

void op_manager_status(op_status_t *s)
{
    uint8_t count = m.plan_n ? m.plan_n : 1;
    s->running = op_manager_busy();
    s->op_id = m.state == OPS_IDLE ? 0 : m.state == OPS_QUEUED ? m.last_id : m.res.op_id;
    s->op_type = m.state == OPS_IDLE ? 0 : m.res.op_type;
    s->stage = m.res.stage;
    s->percent_total = m.state == OPS_DONE ? 100 : (uint8_t)(((uint32_t)m.index * 100u + m.percent) / count);
}

op_result_t *op_current(void) { return &m.res; }
const op_request_t *op_request(void) { return &m.req; }

/* ---------------------------------------------------------------- progress */

static void send_progress(void)
{
    uint8_t p[7];
    uint8_t count = m.plan_n ? m.plan_n : 1;
    put16(p, m.res.op_id);
    p[2] = m.res.op_type;
    p[3] = m.res.stage;
    p[4] = m.index;
    p[5] = count;
    p[6] = (uint8_t)(((uint32_t)m.index * 100u + m.percent) / count);
    session_send_event(PDK_EVT_PROGRESS, p, sizeof p);
    indication_busy(m.res.stage, p[6]);
    m.progress_ms = hal_time_ms();
}

void op_plan(const uint8_t *stages, uint8_t count)
{
    if (count > MAX_STAGES)
        count = MAX_STAGES;
    memcpy(m.plan, stages, count);
    m.plan_n = count;
}

void op_stage(uint8_t stage)
{
    uint8_t i = m.res.stage ? m.index : 0;
    while (i < m.plan_n && m.plan[i] != stage)
        i++;
    if (i == m.plan_n && m.plan_n < MAX_STAGES)
        m.plan[m.plan_n++] = stage;
    m.index = i < MAX_STAGES ? i : (uint8_t)(MAX_STAGES - 1u);
    m.res.stage = stage;
    m.percent = 0;
    send_progress();
}

void op_percent(uint8_t percent) { m.percent = percent > 100 ? 100 : percent; }

bool op_poll(void)
{
    session_poll();        /* commands allowed during an operation are answered, the rest get BUSY */
    housekeeping_poll();
    hal_ui_poll();         /* local cancel */
    if (hal_time_ms() - m.progress_ms >= OP_PROGRESS_INTERVAL_MS)
        send_progress();
    return !m.cancel;
}

/* ---------------------------------------------------------------- RESULT */

uint16_t op_result_encode(const op_result_t *r, uint8_t *out)
{
    uint8_t *p = out;
    put16(p, r->op_id);
    p[2] = r->op_type;
    p[3] = r->source;
    put16(p + 4, r->status);
    p[6] = r->stage;
    put16(p + 7, r->chip_id_found);
    p[9] = r->family_found;
    put16(p + 10, r->fail_addr);
    put16(p + 12, r->expected);
    put16(p + 14, r->actual);
    p[16] = r->serial_valid;
    memcpy(p + 17, r->serial, 8);
    put16(p + 25, r->fuse_written);
    put32(p + 27, r->data_words);
    put32(p + 31, r->data_crc32);
    put32(p + 35, r->elapsed_ms);
    p[39] = r->calib_count;
    p += 40;
    for (uint8_t i = 0; i < r->calib_count; i++, p += 13) {
        const op_calib_result_t *c = &r->calib[i];
        p[0] = c->kind;
        p[1] = c->value;
        put32(p + 2, c->target_hz);
        put32(p + 6, c->measured_hz);
        p[10] = c->mode;
        p[11] = c->master_value;
        p[12] = c->site_status;
    }
    put32(p, r->image_crc_raw);
    put32(p + 4, r->image_crc_norm);
    p[8] = r->warnings;
    p[9] = r->v_rail;
    put16(p + 10, r->v_set_mV);
    put16(p + 12, r->v_meas_mV);
    p[14] = r->level_count;
    p += 15;
    for (uint8_t i = 0; i < r->level_count; i++, p += 8) {
        const op_level_t *l = &r->levels[i];
        p[0] = l->rail;
        put16(p + 1, l->set_mV);
        put16(p + 3, l->meas_mV);
        put16(p + 5, l->settle_us);
        p[7] = l->ok;
    }
    return (uint16_t)(p - out);
}

/* ---------------------------------------------------------------- running */

void op_manager_poll(void)
{
    if (m.state != OPS_QUEUED)
        return;
    uint16_t id = m.last_id;
    memset(&m.res, 0, sizeof m.res);
    m.res.op_id = id;
    m.res.op_type = m.req.op_type;
    m.res.source = m.req.source;
    m.res.fail_addr = 0xFFFF;
    m.res.fuse_written = 0xFFFF;
    m.res.v_rail = 0xFF;
    m.cancel = false;
    m.plan_n = m.index = m.percent = 0;
    m.state = OPS_RUNNING;
    m.start_ms = m.progress_ms = hal_time_ms();

    uint8_t started[4];
    put16(started, id);
    started[2] = m.req.op_type;
    started[3] = m.req.source;
    session_send_event(PDK_EVT_OP_STARTED, started, sizeof started);
    indication_busy(0, 0);

    uint16_t st;
    switch (m.req.op_type) {
    case PDK_OP_PROBE: st = op_probe_run(); break;
    case PDK_OP_ERASE: st = op_erase_run(); break;
    case PDK_OP_BLANK_CHECK: st = op_blank_check_run(); break;
    case PDK_OP_VERIFY: st = op_verify_run(); break;
    case PDK_OP_SELF_TEST: st = op_selftest_run(); break;
#ifdef PDK_FEATURE_PATCH
    case PDK_OP_PATCH: st = op_patch_run(); break;
#endif
    default: st = op_program_run(); break;
    }

    chip_engine_safe_state();   /* every way to DONE goes through the safe state */
    m.res.status = st;
    if (st == PDK_VDD_FAULT || st == PDK_VPP_FAULT) {
        const power_fault_t *f = power_last_fault();
        m.res.v_rail = f->rail;
        m.res.v_set_mV = f->set_mV;
        m.res.v_meas_mV = f->meas_mV;
    }
    m.res.elapsed_ms = hal_time_ms() - m.start_ms;
    /* ok/fail count chips: not a PROGRAM refused before the chip was touched (no package, OTP flags) */
    if (m.req.op_type == PDK_OP_PROGRAM && st != PDK_CHIP_ALREADY_PROGRAMMED && m.res.stage != PDK_STAGE_PKG_CHECK)
        package_store_count(st == PDK_OK);
    m.state = OPS_DONE;

    static uint8_t buf[PDK_MAX_PAYLOAD];
    session_send_event(PDK_EVT_OP_DONE, buf, op_result_encode(&m.res, buf));
    indication_done(m.req.op_type, st);
}
