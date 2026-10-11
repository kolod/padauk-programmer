/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host time, CRC, entropy, UID, platform name and indication. */
#include "hal_crc.h"
#include "hal_entropy.h"
#include "hal_platform.h"
#include "hal_time.h"
#include "hal_ui.h"
#include "host.h"
#include "proto/crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static uint64_t now_us64(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000u + (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

uint32_t hal_time_us(void) { return (uint32_t)now_us64(); }
uint32_t hal_time_ms(void) { return (uint32_t)(now_us64() / 1000u); }

void hal_delay_us(uint32_t us)
{
    uint64_t end = now_us64() + us;
    while (now_us64() < end) {
    }
}

void host_sleep_us(uint32_t us)
{
#ifdef _WIN32
    Sleep(us >= 1000 ? us / 1000 : 1);
#else
    usleep(us);
#endif
}

uint32_t hal_crc32_update(uint32_t state, const void *data, size_t len) { return crc32_sw_update(state, data, len); }

const char *hal_platform_name(void) { return "host-sim"; }

bool hal_entropy_raw(uint8_t *buf, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len;
#endif
}

static uint8_t uid[12] = {0x50, 0x44, 0x4B, 0x53, 0x49, 0x4D, 0, 0, 0, 0, 0, 1};

void host_set_uid(const uint8_t u[12]) { memcpy(uid, u, 12); }
void hal_uid(uint8_t out[12]) { memcpy(out, uid, 12); }

void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *d)
{
    static const char *names[] = {"READY", "BUSY", "SUCCESS", "FAIL", "BLOCKED"};
    fprintf(stderr, "ui_state %s stage=%u percent=%u fail=%u status=0x%04x\n",
            (unsigned)state < 5 ? names[state] : "?", d ? d->stage : 0, d ? d->percent : 0,
            d ? (unsigned)d->fail : 0, d ? d->status : 0);
    fflush(stderr);
}
