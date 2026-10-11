/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pattern ported from easy-pdk-programmer-software/fpdkicserial.c,
 * Copyright (C) 2019-2020 freepdk https://free-pdk.github.io (GPL-3.0-or-later).
 */
#include "serial_sites.h"

static const char serial_marker[SERIAL_SITE_LEN] = {'F', 'P', 'S', 'E', 'R', 'I', 'A', 'L'};

uint16_t serial_ret_opcode(uint8_t code_bits)
{
    switch (code_bits) {
    case 13: return 0x0100;
    case 14:
    case 15: return 0x0200;
    case 16: return 0x0F00;
    default: return 0;
    }
}

uint8_t serial_find_sites(uint8_t code_bits, word_get_fn get, void *ctx, uint16_t count,
                          uint16_t *addrs, uint8_t max)
{
    uint16_t ret = serial_ret_opcode(code_bits);
    uint8_t n = 0;
    if (!ret)
        return 0;
    for (uint32_t a = 0; a + SERIAL_SITE_LEN <= count && n < max;) {
        unsigned k = 0;
        for (; k < SERIAL_SITE_LEN; k++) {
            bool def = false;
            uint16_t w = get(ctx, (uint16_t)(a + k), &def);
            if (!def || w != (uint16_t)(ret | (uint8_t)serial_marker[k]))
                break;
        }
        if (k == SERIAL_SITE_LEN) {
            addrs[n++] = (uint16_t)a;
            a += SERIAL_SITE_LEN;
        } else {
            a++;
        }
    }
    return n;
}
