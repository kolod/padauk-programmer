/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RUN (contracts/protocol.md §4, US6): the chip runs on VDD (param, else the package's run_vdd_mV, else
 * 5000 mV; VPP 0) with a UART bridge — EVT_UART_LINK once the chip's 'U' gave the speed, EVT_UART_RX with
 * what it sends, UART_TX towards it. OP_CANCEL is the normal stop (OK); losing the session stops RUN too
 * (HOST_DISCONNECTED), unlike every other operation (FR-023): nobody would see the chip's output or be
 * able to stop it. Either way the lines go to Hi-Z before the voltages are removed. */
#include "op_run.h"

#include "chip_engine.h"
#include "hal_power.h"
#include "hal_uart.h"
#include "op_manager.h"
#include "package/header.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "proto/session.h"
#include "storage/package_store.h"

#define RUN_DEFAULT_MV 5000u
#define RX_CHUNK 64u

static struct {
    bool active;   /* the bridge is up: UART_TX allowed */
    bool lost;     /* the session was reset while RUN ran */
} run;

void op_run_session_reset(void)
{
    if (run.active)
        run.lost = true;
}

bool op_run_active(void) { return run.active; }

static uint16_t run_vdd(void)
{
    uint16_t mV = op_request()->param;
    if (!mV && package_store_state() == PDK_PKG_VALID) {
        uint8_t raw[2];
        package_store_read(22, raw, sizeof raw);   /* header run_vdd_mV */
        mV = le16(raw);
    }
    return mV ? mV : RUN_DEFAULT_MV;
}

uint16_t op_run_run(void)
{
    static const uint8_t stages[] = {PDK_STAGE_RUN};
    op_plan(stages, sizeof stages);
    op_stage(PDK_STAGE_RUN);
    uint16_t mV = run_vdd();
    uint16_t vdd_max, vpp_max;
    hal_power_limits(&vdd_max, &vpp_max);
    uint16_t lines_max = hal_uart_vdd_max_mV();
    if ((vdd_max && mV > vdd_max) || (lines_max && mV > lines_max))
        return PDK_BAD_PAYLOAD;
    op_current()->v_set_mV = mV;

    run.lost = false;
    uint16_t st = chip_engine_run_start(mV);
    if (st != PDK_OK)
        return st;
    hal_uart_start();
    run.active = true;
    bool linked = false;
    for (;;) {
        if (!op_poll())
            break;   /* OP_CANCEL: the normal stop */
        if (run.lost) {
            st = PDK_HOST_DISCONNECTED;
            break;
        }
        uint32_t baud = hal_uart_baud();
        if (baud && !linked) {
            uint8_t b[4];
            put32(b, baud);
            session_send_event(PDK_EVT_UART_LINK, b, sizeof b);
            linked = true;
        }
        uint8_t buf[RX_CHUNK];
        size_t n = hal_uart_read(buf, sizeof buf);
        if (n)
            session_send_event(PDK_EVT_UART_RX, buf, (uint16_t)n);
    }
    run.active = false;
    hal_uart_stop();
    chip_engine_run_stop();
    return st;
}

uint16_t op_run_cmd_uart_tx(const uint8_t *req, uint16_t len, uint8_t *rsp, uint16_t *rsp_len)
{
    (void)rsp;
    (void)rsp_len;
    if (!run.active)
        return PDK_NOT_ALLOWED_NOW;
    if (!len)
        return PDK_BAD_PAYLOAD;
    return hal_uart_write(req, len) ? PDK_OK : PDK_BUSY;   /* no speed yet or the buffer is full: retry */
}
