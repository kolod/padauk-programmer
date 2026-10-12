/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fake_hal.h"

#include <string.h>

#include "hal_crc.h"
#include "hal_entropy.h"
#include "hal_flash.h"
#include "hal_platform.h"
#include "hal_time.h"
#include "hal_transport.h"
#include "proto/crc32.h"

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE (64u * 1024u)

static struct {
    bool open;
    uint8_t rx[65536];
    size_t rx_len, rx_pos;
    uint8_t tx[65536];
    size_t tx_len;

    uint32_t now_us;

    uint8_t flash[FLASH_SIZE];
    uint32_t ops, erases;
    uint32_t fail_after;
    bool fail_armed, failed;
    uint32_t bg_busy_polls;      /* status() answers BUSY this many times after erase_start() */
    uint32_t bg_left, bg_addr;
    hal_erase_status_t bg_result;
    bool bg_violation;           /* the core touched a page while it was erasing */
    uint32_t bg_started;

    uint16_t set[2];
    uint16_t from[2];
    uint32_t set_at[2];
    fake_power_mode_t mode[2];
    uint32_t settle_us;
    uint32_t off_calls;

    pdk_ui_state_t ui;
} f;

static hal_flash_geometry_t geometry = {
    .page_size = 2048u,
    .write_unit = 2,
    .slot_count = 2,
    .slot_addr = {0x0800B000u, 0x0800D000u},
    .slot_size = 8192u,
    .journal_addr = {0x0800F000u, 0x0800F800u},
};

void fake_reset(void)
{
    memset(&f, 0, sizeof f);
    geometry.write_unit = 2;
    memset(f.flash, 0xFF, sizeof f.flash);
    f.open = true;
    f.settle_us = 200;
}

/* ---------------- transport ---------------- */
void fake_set_open(bool open) { f.open = open; }

void fake_rx_push(const uint8_t *data, size_t len)
{
    if (f.rx_pos == f.rx_len)
        f.rx_pos = f.rx_len = 0;
    memcpy(f.rx + f.rx_len, data, len);
    f.rx_len += len;
}

size_t fake_tx_take(uint8_t *buf, size_t cap)
{
    size_t n = f.tx_len < cap ? f.tx_len : cap;
    memcpy(buf, f.tx, n);
    memmove(f.tx, f.tx + n, f.tx_len - n);
    f.tx_len -= n;
    return n;
}

size_t hal_transport_read(uint8_t *buf, size_t max)
{
    size_t n = f.rx_len - f.rx_pos;
    if (n > max)
        n = max;
    memcpy(buf, f.rx + f.rx_pos, n);
    f.rx_pos += n;
    return n;
}

void hal_transport_write(const uint8_t *buf, size_t len)
{
    if (!f.open || f.tx_len + len > sizeof f.tx)
        return;
    memcpy(f.tx + f.tx_len, buf, len);
    f.tx_len += len;
}

bool hal_transport_is_open(void) { return f.open; }

/* ---------------- time ---------------- */
uint32_t fake_now_us(void) { return f.now_us; }
void fake_advance_us(uint32_t us) { f.now_us += us; }
uint32_t hal_time_us(void) { return ++f.now_us; }   /* reading the clock takes time */
uint32_t hal_time_ms(void) { return f.now_us / 1000u; }
void hal_delay_us(uint32_t us) { f.now_us += us; }

/* ---------------- crc / platform / entropy / ui ---------------- */
uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len) { return crc32_sw_update(state, data, len); }
const char *hal_platform_name(void) { return "fake"; }

bool hal_entropy_raw(uint8_t *buf, size_t len)
{
    static uint32_t x = 0x12345678u;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
    return true;
}

void hal_uid(uint8_t uid[12])
{
    for (int i = 0; i < 12; i++)
        uid[i] = (uint8_t)(0xA0 + i);
}

void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *detail)
{
    (void)detail;
    f.ui = state;
}
pdk_ui_state_t fake_ui_last(void) { return f.ui; }

/* ---------------- flash ---------------- */
const hal_flash_geometry_t *hal_flash_geometry(void) { return &geometry; }
uint8_t *fake_flash_mem(uint32_t addr) { return &f.flash[addr - FLASH_BASE]; }
uint32_t fake_flash_ops(void) { return f.ops; }
uint32_t fake_flash_erases(void) { return f.erases; }
bool fake_flash_failed(void) { return f.failed; }

void fake_flash_fail_after(uint32_t n)
{
    f.fail_armed = true;
    f.fail_after = n;
}

static bool touches_erasing(uint32_t addr, size_t len)
{
    return f.bg_left && addr < f.bg_addr + geometry.page_size && addr + len > f.bg_addr;
}

void fake_flash_bg_busy_polls(uint32_t n) { f.bg_busy_polls = n; }
void fake_flash_write_unit(uint8_t unit) { geometry.write_unit = unit; }
bool fake_flash_bg_violation(void) { return f.bg_violation; }
uint32_t fake_flash_bg_started(void) { return f.bg_started; }

bool hal_flash_erase_start(uint32_t addr)
{
    if (f.bg_left)
        f.bg_violation = true;   /* only one background erase at a time */
    f.bg_started++;
    bool ok = hal_flash_erase_page(addr);
    f.bg_result = ok ? HAL_ERASE_DONE : HAL_ERASE_FAILED;
    f.bg_addr = addr;
    f.bg_left = f.bg_busy_polls + 1u;
    return ok;
}

hal_erase_status_t hal_flash_erase_status(void)
{
    if (!f.bg_left)
        return HAL_ERASE_IDLE;
    if (--f.bg_left)
        return HAL_ERASE_BUSY;
    return f.bg_result;
}

void fake_flash_power_restored(void)
{
    f.fail_armed = false;
    f.failed = false;
}

/* Returns 0 = do the op, 1 = tear it (do half), 2 = refuse. */
static int fail_check(void)
{
    if (f.failed)
        return 2;
    if (f.fail_armed) {
        if (f.fail_after == 0) {
            f.failed = true;
            return 1;
        }
        f.fail_after--;
    }
    return 0;
}

bool hal_flash_erase_page(uint32_t addr)
{
    if (f.bg_left > 1)
        f.bg_violation = true;   /* the core must wait for the background erase first */
    if (addr < FLASH_BASE || addr + geometry.page_size > FLASH_BASE + FLASH_SIZE || (addr - FLASH_BASE) % geometry.page_size)
        return false;
    int fc = fail_check();
    if (fc == 2)
        return false;
    f.ops++;
    f.erases++;
    memset(fake_flash_mem(addr), 0xFF, fc == 1 ? geometry.page_size / 2 : geometry.page_size);
    return fc == 0;
}

bool hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (touches_erasing(addr, len))
        f.bg_violation = true;
    if ((addr | len) % geometry.write_unit || addr < FLASH_BASE || addr + len > FLASH_BASE + FLASH_SIZE)
        return false;
    int fc = fail_check();
    if (fc == 2)
        return false;
    f.ops++;
    uint8_t *m = fake_flash_mem(addr);
    size_t n = fc == 1 ? (len / 4u) * 2u : len;
    for (size_t i = 0; i < n; i += 2) {
        if (m[i] != 0xFF || m[i + 1] != 0xFF)
            return false;   /* half-words must be erased (STM32F0 rule) */
        m[i] = data[i];
        m[i + 1] = data[i + 1];
    }
    return fc == 0;
}

void hal_flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (touches_erasing(addr, len))
        f.bg_violation = true;
    memcpy(buf, fake_flash_mem(addr), len);
}

/* ---------------- power ---------------- */
void fake_power_mode(pdk_rail_t rail, fake_power_mode_t mode) { f.mode[rail] = mode; }
void fake_power_settle_us(uint32_t us) { f.settle_us = us; }
uint16_t fake_power_set_value(pdk_rail_t rail) { return f.set[rail]; }
uint32_t fake_power_off_calls(void) { return f.off_calls; }

/* Ideal ramp from the previous level to the target (fault modes applied in measure). */
static uint16_t ideal(pdk_rail_t rail)
{
    uint32_t settle = f.mode[rail] == FAKE_POWER_SLOW ? 50000u : f.settle_us;
    uint32_t dt = f.now_us - f.set_at[rail];
    if (dt >= settle)
        return f.set[rail];
    return (uint16_t)(f.from[rail] + ((int32_t)f.set[rail] - f.from[rail]) * (int32_t)dt / (int32_t)settle);
}

void hal_power_set(pdk_rail_t rail, uint16_t mV)
{
    f.from[rail] = ideal(rail);
    f.set[rail] = mV;
    f.set_at[rail] = f.now_us;
}

uint16_t hal_power_measure(pdk_rail_t rail)
{
    f.now_us += 10;   /* a measurement takes time */
    switch (f.mode[rail]) {
    case FAKE_POWER_STUCK: return f.from[rail];
    case FAKE_POWER_LOW: return (uint16_t)(f.set[rail] * 9u / 10u);
    default: return ideal(rail);
    }
}

void hal_power_off_all(void)
{
    f.off_calls++;
    f.set[0] = f.set[1] = 0;
    f.from[0] = f.from[1] = 0;
    f.set_at[0] = f.set_at[1] = f.now_us;
}

void hal_power_limits(uint16_t *vdd_max_mV, uint16_t *vpp_max_mV)
{
    *vdd_max_mV = 6260;
    *vpp_max_mV = 13270;
}
