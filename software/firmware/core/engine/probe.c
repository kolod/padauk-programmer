/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Auto-detection at the identification voltages (docs/easy-pdk-programmer-protocol.md §4.6): one enter/leave per family
 * at VPP 4.5 V / VDD 2.0 V — FLASH (read command, 16-bit ACK), then OTP1_2, OTP2_2, OTP2_1, OTP3_1 (write command, ID bits
 * read as for a 16-bit core) — and the free-pdk host's interpretation of the answer against the chip table. No voltage
 * above these levels is applied here. */
#include "engine_internal.h"
#include "proto/proto_defs.h"
#include "proto/trace.h"

/* A chip of `family` with `code_bits` whose 12-bit ID is `id`. */
static const chip_info_t *find(uint16_t id, uint8_t family, uint8_t code_bits)
{
    for (size_t i = 0; i < chip_count(); i++) {
        const chip_info_t *c = chip_at(i);
        if (c->id12 == id && c->family == family && (!code_bits || c->code_bits == code_bits))
            return c;
    }
    return NULL;
}

static uint16_t attempt(uint8_t family, uint8_t cmd, uint32_t *rsp, chip_poll_fn poll)
{
    uint16_t st = eng_enter(NULL, family, MODE_PROBE, ENG_PROBE_VPP_MV, ENG_PROBE_VDD_MV, poll);
    if (st != PDK_OK)
        return st;
    fam_command(family, cmd);
    uint8_t bits = fam_is_flash(family) ? 16 : fam_otp_id_len(family, 16);
    if (fam_is_flash(family))
        *rsp = fam_flash_ack(CHIP_FLASH_1);   /* 16 bits: a FLASH_2 answer arrives shifted by one */
    else
        *rsp = fam_otp_id_bits(family, bits);
    trace_emit(TRACE_PROBE, family, 0, bits, *rsp);
    eng_leave();
    return PDK_OK;
}

uint16_t chip_engine_probe(uint16_t *id12, uint8_t *family, chip_poll_fn poll)
{
    *id12 = 0;
    *family = 0;
    eng_leave();
    uint32_t rsp = 0;
    uint16_t st = attempt(CHIP_FLASH_1, CMD_READ, &rsp, poll);
    if (st != PDK_OK)
        return st;
    if (rsp & 0xFFFFu) {
        const chip_info_t *c = find((uint16_t)(rsp & 0xFFFu), CHIP_FLASH_1, 0);
        if (!c)
            c = find((uint16_t)((rsp << 1) & 0xFFFu), CHIP_FLASH_2, 0);
        *id12 = c ? c->id12 : (uint16_t)(rsp & 0xFFFu);
        *family = c ? c->family : CHIP_FLASH_1;
        return PDK_OK;
    }
    static const uint8_t otp[] = {CHIP_OTP1_2, CHIP_OTP2_2, CHIP_OTP2_1, CHIP_OTP3_1};
    for (unsigned i = 0; i < sizeof otp; i++) {
        if ((st = attempt(otp[i], CMD_WRITE, &rsp, poll)) != PDK_OK)
            return st;
        if (!(rsp & 0xFFFFu))
            continue;
        unsigned k = otp[i] == CHIP_OTP1_2 || otp[i] == CHIP_OTP2_2 ? 2u : 1u;
        for (uint8_t bits = 16; bits >= 13; bits--) {
            uint16_t id = (uint16_t)((rsp >> (k * (16u - bits))) & 0xFFFu);
            const chip_info_t *c = find(id, otp[i], bits);
            if (c) {
                *id12 = c->id12;
                *family = c->family;
                return PDK_OK;
            }
        }
        *id12 = (uint16_t)(rsp & 0xFFFu);   /* an answer, but no chip of the table */
        *family = otp[i];
        return PDK_OK;
    }
    return PDK_CHIP_NOT_FOUND;
}
