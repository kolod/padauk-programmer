/* SPDX-License-Identifier: GPL-3.0-or-later */
/* State journal (data-model StateRecord, research R6). */
#include "pdk_test.h"

#include "fake_hal.h"
#include "hal_flash.h"
#include "proto/le.h"
#include "storage/journal.h"

#define PER_PAGE (2048u / JOURNAL_RECORD_SIZE)

void pdk_test_setup(void)
{
    fake_reset();
    journal_init();
}

static journal_state_t state(uint32_t ok)
{
    journal_state_t s = *journal_get();
    s.active_slot = (uint8_t)(ok & 1u);
    s.package_size = 140;
    s.package_crc = 0xC0DE0000u + ok;
    s.ok_count = ok;
    return s;
}

static uint32_t page_addr(int p) { return hal_flash_geometry()->journal_addr[p]; }

TEST(encode_decode_and_crc_bytes_0_35)
{
    journal_state_t s = {.seq = 7, .active_slot = 1, .cfg_copy_ihrc = 2, .cfg_copy_ilrc = 0, .cfg_copy_bg = 1,
                         .flags = 1, .package_size = 140, .package_crc = 0xAABBCCDD, .ok_count = 5, .fail_count = 3};
    uint8_t rec[JOURNAL_RECORD_SIZE];
    journal_record_encode(&s, rec);
    ASSERT_MEM_EQ(rec, "PDKS", 4);
    ASSERT_EQ(le32(rec + 4), 7);
    ASSERT_EQ(rec[8], 1);
    ASSERT_EQ(rec[9], 2);
    ASSERT_EQ(le16(rec + 10), 1);
    ASSERT_EQ(le32(rec + 12), 140);
    ASSERT_EQ(le32(rec + 16), 0xAABBCCDD);
    ASSERT_EQ(le32(rec + 24), 5);
    ASSERT_EQ(le32(rec + 28), 3);
    ASSERT_EQ(rec[32], 0);
    ASSERT_EQ(rec[33], 1);
    journal_state_t d;
    ASSERT_TRUE(journal_record_decode(rec, &d));
    ASSERT_EQ(d.package_crc, s.package_crc);
    ASSERT_EQ(d.fail_count, 3);
    for (int i = 0; i < (int)JOURNAL_RECORD_SIZE; i++) {
        uint8_t bad[JOURNAL_RECORD_SIZE];
        memcpy(bad, rec, sizeof bad);
        bad[i] ^= 0x10;
        if (journal_record_decode(bad, &d))
            PDK_FAIL("corruption of byte %d not detected", i);
    }
}

TEST(empty_flash_gives_defaults)
{
    const journal_state_t *s = journal_get();
    ASSERT_EQ(s->seq, 0);
    ASSERT_EQ(s->active_slot, JOURNAL_NO_SLOT);
    ASSERT_EQ(s->cfg_copy_ihrc, 0);
    ASSERT_EQ(s->cfg_copy_ilrc, 0);
    ASSERT_EQ(s->cfg_copy_bg, 1);
    ASSERT_EQ(s->flags, 0);
}

TEST(latest_record_wins_after_restart)
{
    for (uint32_t i = 1; i <= 5; i++) {
        journal_state_t s = state(i);
        ASSERT_TRUE(journal_append(&s));
        ASSERT_EQ(journal_get()->seq, i);
    }
    journal_init();
    ASSERT_EQ(journal_get()->seq, 5);
    ASSERT_EQ(journal_get()->ok_count, 5);
    ASSERT_EQ(journal_get()->package_crc, 0xC0DE0005u);
}

TEST(torn_record_is_ignored)
{
    for (uint32_t i = 1; i <= 3; i++) {
        journal_state_t s = state(i);
        ASSERT_TRUE(journal_append(&s));
    }
    fake_flash_fail_after(0);
    journal_state_t s = state(4);
    ASSERT_TRUE(!journal_append(&s));
    fake_flash_power_restored();
    journal_init();
    ASSERT_EQ(journal_get()->seq, 3);
    ASSERT_EQ(journal_get()->ok_count, 3);
    s = state(4);
    ASSERT_TRUE(journal_append(&s));   /* the torn slot is skipped, not overwritten */
    journal_init();
    ASSERT_EQ(journal_get()->seq, 4);
    ASSERT_EQ(journal_get()->ok_count, 4);
}

TEST(page_overflow_switches_pages)
{
    for (uint32_t i = 1; i <= PER_PAGE; i++) {
        journal_state_t s = state(i);
        ASSERT_TRUE(journal_append(&s));
    }
    ASSERT_EQ(fake_flash_erases(), 0);   /* both pages start erased */
    journal_state_t s = state(PER_PAGE + 1);
    ASSERT_TRUE(journal_append(&s));
    ASSERT_EQ(fake_flash_erases(), 1);
    ASSERT_EQ(le32(fake_flash_mem(page_addr(1)) + 4), PER_PAGE + 1);
    journal_init();
    ASSERT_EQ(journal_get()->seq, PER_PAGE + 1);

    /* fill page 1 and wrap back to page 0 */
    for (uint32_t i = PER_PAGE + 2; i <= 2 * PER_PAGE + 1; i++) {
        s = state(i);
        ASSERT_TRUE(journal_append(&s));
    }
    ASSERT_EQ(fake_flash_erases(), 2);
    ASSERT_EQ(le32(fake_flash_mem(page_addr(0)) + 4), 2 * PER_PAGE + 1);
    journal_init();
    ASSERT_EQ(journal_get()->seq, 2 * PER_PAGE + 1);
    ASSERT_EQ(journal_get()->ok_count, 2 * PER_PAGE + 1);
}

TEST(power_loss_while_erasing_second_page)
{
    for (uint32_t i = 1; i <= PER_PAGE; i++) {
        journal_state_t s = state(i);
        ASSERT_TRUE(journal_append(&s));
    }
    /* fill page 1 with an older generation so a torn erase leaves stale records there */
    for (uint32_t i = 0; i < PER_PAGE; i++) {
        journal_state_t old = state(0);
        old.seq = i + 1000u - 2000u;   /* "older" by serial arithmetic */
        uint8_t rec[JOURNAL_RECORD_SIZE];
        journal_record_encode(&old, rec);
        memcpy(fake_flash_mem(page_addr(1) + i * JOURNAL_RECORD_SIZE), rec, sizeof rec);
    }
    journal_init();
    ASSERT_EQ(journal_get()->seq, PER_PAGE);

    fake_flash_fail_after(0);
    journal_state_t s = state(PER_PAGE + 1);
    ASSERT_TRUE(!journal_append(&s));
    fake_flash_power_restored();
    journal_init();
    ASSERT_EQ(journal_get()->seq, PER_PAGE);           /* page 0 still holds the current state */
    ASSERT_EQ(journal_get()->ok_count, PER_PAGE);
    s = state(PER_PAGE + 1);
    ASSERT_TRUE(journal_append(&s));
    journal_init();
    ASSERT_EQ(journal_get()->seq, PER_PAGE + 1);
}

TEST(seq_compare_survives_wraparound)
{
    journal_state_t a = state(1), b = state(2);
    a.seq = 0xFFFFFFFFu;
    b.seq = 0;   /* newer than 0xFFFFFFFF */
    uint8_t rec[JOURNAL_RECORD_SIZE];
    journal_record_encode(&b, rec);
    memcpy(fake_flash_mem(page_addr(0)), rec, sizeof rec);
    journal_record_encode(&a, rec);
    memcpy(fake_flash_mem(page_addr(1)), rec, sizeof rec);
    journal_init();
    ASSERT_EQ(journal_get()->seq, 0);
    ASSERT_EQ(journal_get()->ok_count, 2);
}
