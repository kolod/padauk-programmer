/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RUN with the UART bridge (T121): VDD from param / 5000 mV with VPP 0, EVT_UART_LINK once the speed is
 * known, EVT_UART_RX, UART_TX to the chip (NOT_ALLOWED_NOW outside RUN, BUSY when full), STATUS running,
 * OP_CANCEL → OK and session loss → HOST_DISCONNECTED, both with the bridge stopped and voltages off. */
#include "pdk_test.h"

#include <string.h>

#include "fake_chip.h"
#include "fake_hal.h"
#include "fake_uart.h"
#include "ops/op_manager.h"
#include "pdk_core.h"
#include "proto/le.h"
#include "proto/proto_defs.h"
#include "test_ops.h"

static unsigned passes;
static uint16_t vdd_seen, vpp_seen;
static uint16_t op_id;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
    pdk_core_poll();
    tops_seq = 1;
    tp_hello(tops_seq++);
    passes = 0;
    vdd_seen = vpp_seen = 0;
}

static void watch(void)
{
    passes++;
    vdd_seen = fake_power_set_value(PDK_RAIL_VDD);
    vpp_seen = fake_power_set_value(PDK_RAIL_VPP);
}

/* Sends OP_START(RUN, param) and runs until the hook stops it; returns the frames sent meanwhile. */
static int run(uint16_t param, tp_frame_t *frames, int max)
{
    uint8_t start[6] = {PDK_OP_RUN, 0, 0, 0, 0, 0};
    put16(start + 4, param);
    tp_send(tops_seq++, PDK_CMD_OP_START, start, sizeof start);
    int n = tp_collect(frames, max);
    for (int i = 0; i < n; i++)
        if (frames[i].code == PDK_CMD_OP_START && frames[i].type == PDK_FT_RSP)
            op_id = le16(frames[i].payload);
    return n;
}

static const tp_frame_t *find(const tp_frame_t *f, int n, uint16_t code)
{
    for (int i = 0; i < n; i++)
        if (f[i].code == code)
            return &f[i];
    return NULL;
}

static uint16_t tx_seq, status_seq;

static void talk_then_cancel(void)
{
    watch();
    if (passes == 2) {
        tx_seq = tops_seq++;
        tp_send(tx_seq, PDK_CMD_UART_TX, "abc", 3);
        status_seq = tops_seq++;
        tp_send(status_seq, PDK_CMD_GET_STATUS, NULL, 0);
    }
    if (passes == 5) {
        op_status_t s;
        op_manager_status(&s);
        op_manager_cancel(s.op_id);
    }
}

static tp_frame_t frames[64];

TEST(run_bridges_the_uart_until_cancel)
{
    fake_uart_baud(9600);
    fake_uart_chip_sends("hello\n", 6);
    fake_uart_hook(talk_then_cancel);
    int n = run(0, frames, 64);
    fake_uart_hook(NULL);
    ASSERT_EQ(vdd_seen, 5000);   /* no param, no package: 5000 mV */
    ASSERT_EQ(vpp_seen, 0);
    const tp_frame_t *link = find(frames, n, PDK_EVT_UART_LINK);
    ASSERT_TRUE(link != NULL);
    ASSERT_EQ(le32(link->payload), 9600u);
    const tp_frame_t *rx = find(frames, n, PDK_EVT_UART_RX);
    ASSERT_TRUE(rx != NULL);
    ASSERT_EQ(rx->len, 6);
    ASSERT_TRUE(!memcmp(rx->payload, "hello\n", 6));
    bool tx_ok = false, running = false;
    for (int i = 0; i < n; i++) {
        if (frames[i].type == PDK_FT_RSP && frames[i].seq == tx_seq)
            tx_ok = frames[i].status == PDK_OK;
        if (frames[i].type == PDK_FT_RSP && frames[i].seq == status_seq)
            running = frames[i].payload[0] == PDK_STATE_RUNNING;
    }
    ASSERT_TRUE(tx_ok);
    ASSERT_TRUE(running);
    uint8_t got[8];
    ASSERT_EQ(fake_uart_take_tx(got, sizeof got), 3u);
    ASSERT_TRUE(!memcmp(got, "abc", 3));

    const op_result_t *r = op_manager_result(op_id);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->op_type, PDK_OP_RUN);
    ASSERT_EQ(r->status, PDK_OK);   /* OP_CANCEL is the normal stop of RUN */
    ASSERT_EQ(r->stage, PDK_STAGE_RUN);
    ASSERT_TRUE(!fake_uart_on());
    ASSERT_EQ(fake_uart_stops(), 1u);
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);

    tp_frame_t rsp;   /* the bridge is gone */
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_UART_TX, "x", 1, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_NOT_ALLOWED_NOW);
}

static void lose_session(void)
{
    watch();
    if (passes == 3)
        fake_set_open(false);   /* USB gone / DTR down */
}

TEST(session_loss_stops_run)
{
    fake_uart_baud(115200);
    fake_uart_hook(lose_session);
    run(3300, frames, 64);
    fake_uart_hook(NULL);
    ASSERT_EQ(vdd_seen, 3300);
    const op_result_t *r = op_manager_result(op_id);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->status, PDK_HOST_DISCONNECTED);
    ASSERT_TRUE(!fake_uart_on());
    ASSERT_EQ(fake_power_set_value(PDK_RAIL_VDD), 0);
}

static void cancel_at_3(void)
{
    watch();
    if (passes == 3) {
        op_status_t s;
        op_manager_status(&s);
        op_manager_cancel(s.op_id);
    }
}

static uint16_t busy_status;

static void tx_before_the_speed(void)
{
    watch();
    if (passes == 1) {
        tx_seq = tops_seq++;
        tp_send(tx_seq, PDK_CMD_UART_TX, "a", 1);
    }
    if (passes == 3) {
        op_status_t s;
        op_manager_status(&s);
        op_manager_cancel(s.op_id);
    }
}

TEST(no_link_event_before_the_speed_and_tx_is_busy)
{
    fake_uart_hook(tx_before_the_speed);   /* the chip never sent its 'U' */
    int n = run(0, frames, 64);
    fake_uart_hook(NULL);
    ASSERT_TRUE(find(frames, n, PDK_EVT_UART_LINK) == NULL);
    busy_status = 0;
    for (int i = 0; i < n; i++)
        if (frames[i].type == PDK_FT_RSP && frames[i].seq == tx_seq)
            busy_status = frames[i].status;
    ASSERT_EQ(busy_status, PDK_BUSY);
}

TEST(vdd_above_the_board_limit_is_refused)
{
    fake_uart_hook(cancel_at_3);
    run(7000, frames, 64);
    fake_uart_hook(NULL);
    const op_result_t *r = op_manager_result(op_id);
    ASSERT_EQ(r->status, PDK_BAD_PAYLOAD);
    ASSERT_EQ(passes, 0u);   /* never powered */
    ASSERT_EQ(fake_uart_starts(), 0u);
}

TEST(uart_tx_outside_run)
{
    tp_frame_t rsp;
    ASSERT_EQ(tp_call(tops_seq++, PDK_CMD_UART_TX, "x", 1, &rsp), 0);
    ASSERT_EQ(rsp.status, PDK_NOT_ALLOWED_NOW);
}
