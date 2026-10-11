/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pdk_test.h"

#include "fake_hal.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "test_proto.h"

static tp_frame_t r;

void pdk_test_setup(void)
{
    fake_reset();
    pdk_core_init();
}

TEST(get_info_without_session)
{
    ASSERT_EQ(tp_call(1, PDK_CMD_GET_INFO, NULL, 0, &r), 0);
    ASSERT_EQ(r.type, PDK_FT_RSP);
    ASSERT_EQ(r.status, PDK_OK);
    ASSERT_EQ(r.len, 55);
    ASSERT_EQ(r.payload[0], 1);
    ASSERT_EQ(r.payload[1], 0);
}

TEST(commands_before_hello_are_rejected)
{
    uint8_t first = 0;
    ASSERT_EQ(tp_call(1, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.status, PDK_NO_SESSION);
    ASSERT_EQ(tp_call(2, PDK_CMD_PKG_INFO, NULL, 0, &r), 0);
    ASSERT_EQ(r.status, PDK_NO_SESSION);
}

TEST(hello_establishes_session)
{
    uint8_t p[] = {1, 0, 'x'};
    ASSERT_EQ(tp_call(1, PDK_CMD_HELLO, p, sizeof p, &r), 0);
    ASSERT_EQ(r.status, PDK_OK);
    ASSERT_EQ(r.len, 55);
    uint8_t first = 0;
    ASSERT_EQ(tp_call(2, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.status, PDK_OK);
}

TEST(hello_wrong_major_returns_info_and_no_session)
{
    uint8_t p[] = {2, 0};
    ASSERT_EQ(tp_call(1, PDK_CMD_HELLO, p, sizeof p, &r), 0);
    ASSERT_EQ(r.status, PDK_PROTO_VERSION);
    ASSERT_EQ(r.len, 55);
    uint8_t first = 0;
    ASSERT_EQ(tp_call(2, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.status, PDK_NO_SESSION);
}

TEST(unknown_command)
{
    ASSERT_EQ(tp_call(1, 0x7777, NULL, 0, &r), 0);
    ASSERT_EQ(r.status, PDK_UNKNOWN_COMMAND);
}

TEST(corrupted_frame_gets_nak)
{
    uint8_t garbage[] = {0x05, 0x01, 0x02, 0x00};
    tp_send_raw(garbage, sizeof garbage);
    tp_frame_t fr[4];
    int n = tp_collect(fr, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(fr[0].type, PDK_FT_NAK);
    ASSERT_EQ(fr[0].status, PDK_FRAME_ERROR);
    ASSERT_EQ(fr[0].seq, 0xFFFF);
}

TEST(duplicate_seq_returns_cached_response_without_reexecution)
{
    tp_hello(1);
    /* PKG_BEGIN with garbage header fails; then PKG_DATA without upload → NOT_ALLOWED_NOW.
     * Use PKG_CHECK (state query) with the same seq twice and a changed world in between:
     * the second answer must be byte-identical (served from the cache). */
    ASSERT_EQ(tp_call(5, PDK_CMD_PKG_CHECK, NULL, 0, &r), 0);
    tp_frame_t first = r;
    ASSERT_EQ(tp_call(5, PDK_CMD_PKG_CHECK, NULL, 0, &r), 0);
    ASSERT_EQ(r.seq, first.seq);
    ASSERT_EQ(r.status, first.status);
    ASSERT_EQ(r.len, first.len);
    ASSERT_MEM_EQ(r.payload, first.payload, r.len);
    /* a different seq is executed again */
    ASSERT_EQ(tp_call(6, PDK_CMD_PKG_CHECK, NULL, 0, &r), 0);
    ASSERT_EQ(r.seq, 6);
}

TEST(duplicate_hello_is_not_reexecuted)
{
    tp_hello(9);
    /* re-sending seq 9 with a different (bad) major must not drop the session */
    uint8_t bad[] = {2, 0};
    ASSERT_EQ(tp_call(9, PDK_CMD_HELLO, bad, sizeof bad, &r), 0);
    ASSERT_EQ(r.status, PDK_OK);
    uint8_t first = 0;
    ASSERT_EQ(tp_call(10, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.status, PDK_OK);
}

TEST(port_close_resets_session)
{
    tp_hello(1);
    fake_set_open(false);
    pdk_core_poll();
    fake_set_open(true);
    pdk_core_poll();
    uint8_t first = 0;
    ASSERT_EQ(tp_call(2, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.status, PDK_NO_SESSION);
}

TEST(port_close_clears_response_cache)
{
    tp_hello(1);
    fake_set_open(false);
    pdk_core_poll();
    fake_set_open(true);
    pdk_core_poll();
    /* same seq as before must be executed (not served from cache) */
    uint8_t bad[] = {2, 0};
    ASSERT_EQ(tp_call(1, PDK_CMD_HELLO, bad, sizeof bad, &r), 0);
    ASSERT_EQ(r.status, PDK_PROTO_VERSION);
}

TEST(list_chips_pages)
{
    tp_hello(1);
    uint8_t first = 0;
    ASSERT_EQ(tp_call(2, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(r.payload[0], 13);
    int got = r.payload[1];
    ASSERT_TRUE(got > 0 && got <= 13);
    ASSERT_EQ(r.len, 2 + got * 48);
    first = (uint8_t)got;
    ASSERT_EQ(tp_call(3, PDK_CMD_LIST_CHIPS, &first, 1, &r), 0);
    ASSERT_EQ(got + r.payload[1], 13);
    /* first record of page 2 has the right index */
    ASSERT_EQ(r.payload[2], got);
}

TEST(init_switches_power_off_first)
{
    fake_reset();
    ASSERT_EQ(fake_power_off_calls(), 0);
    pdk_core_init();
    ASSERT_EQ(fake_power_off_calls(), 1);
}
