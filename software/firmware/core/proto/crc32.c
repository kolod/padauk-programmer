/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crc32.h"

#include "hal_crc.h"

static const uint32_t crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t crc32_sw_update(uint32_t state, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        state ^= *p++;
        state = (state >> 4) ^ crc32_nibble[state & 0x0Fu];
        state = (state >> 4) ^ crc32_nibble[state & 0x0Fu];
    }
    return state;
}

uint32_t crc32_update(uint32_t state, const void *data, size_t len)
{
    return hal_crc32_update(state, data, len);
}

uint32_t crc32_compute(const void *data, size_t len)
{
    return crc32_final(crc32_update(crc32_init(), data, len));
}
