/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pdk-sim: the programmer core on the PC, speaking the protocol over TCP.
 *
 *   pdk-sim [--tcp HOST:PORT] [--flash FILE] [--power-fail-after N] [--power-fault RAIL:MODE]
 *           [--power-scale RAIL:OUT:MEAS] [--corrupt-flash-during-write] [--uid HEX24]
 *
 * Exit code 99 = simulated power loss. Prints "listening HOST:PORT" on stdout when ready.
 */
#include "hal_ui.h"
#include "host.h"
#include "pdk_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "usage: pdk-sim [--tcp HOST:PORT] [--flash FILE] [--power-fail-after N]\n"
                    "               [--power-fault vdd|vpp:low|slow|stuck] [--power-scale vdd|vpp:OUT:MEAS]\n"
                    "               [--corrupt-flash-during-write] [--uid HEX24]\n");
    exit(2);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    unsigned port = 5555;
    const char *flash = NULL;
    static char hostbuf[64];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--tcp") && v) {
            const char *colon = strrchr(v, ':');
            if (!colon || (size_t)(colon - v) >= sizeof hostbuf)
                usage();
            memcpy(hostbuf, v, (size_t)(colon - v));
            hostbuf[colon - v] = 0;
            host = hostbuf;
            port = (unsigned)atoi(colon + 1);
            i++;
        } else if (!strcmp(a, "--flash") && v) {
            flash = v;
            i++;
        } else if (!strcmp(a, "--power-fail-after") && v) {
            host_flash_power_fail_after(atol(v));
            i++;
        } else if (!strcmp(a, "--power-fault") && v) {
            if (!host_power_fault(v))
                usage();
            i++;
        } else if (!strcmp(a, "--power-scale") && v) {
            if (!host_power_scale(v))
                usage();
            i++;
        } else if (!strcmp(a, "--corrupt-flash-during-write")) {
            host_flash_corrupt_during_write(true);
        } else if (!strcmp(a, "--uid") && v && strlen(v) == 24) {
            uint8_t uid[12];
            for (int k = 0; k < 12; k++) {
                int hi = hexval(v[2 * k]), lo = hexval(v[2 * k + 1]);
                if (hi < 0 || lo < 0)
                    usage();
                uid[k] = (uint8_t)(hi << 4 | lo);
            }
            host_set_uid(uid);
            i++;
        } else {
            usage();
        }
    }

    if (!host_flash_open(flash)) {
        fprintf(stderr, "cannot open flash file %s\n", flash);
        return 1;
    }
    if (!host_transport_listen(host, (uint16_t)port)) {
        fprintf(stderr, "cannot listen on %s:%u\n", host, port);
        return 1;
    }
    pdk_core_init();
    printf("listening %s:%u\n", host, port);
    fflush(stdout);

    for (;;) {
        pdk_core_poll();
        host_sleep_us(200);
    }
}
