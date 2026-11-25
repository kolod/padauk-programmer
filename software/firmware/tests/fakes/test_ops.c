/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_ops.h"

#include <string.h>

#include "chips/calib_footprint.h"
#include "chips/serial_sites.h"
#include "pdk_core.h"
#include "proto/proto_defs.h"
#include "test_pkg.h"

uint16_t tops_seq = 1;

const chip_info_t *tops_chip(const char *name)
{
    for (size_t i = 0; i < chip_count(); i++)
        if (chip_name_matches(chip_at(i), name))
            return chip_at(i);
    return NULL;
}

void tops_image_init(tops_image_t *img) { memset(img, 0, sizeof *img); }

void tops_put(tops_image_t *img, uint16_t addr, uint16_t w)
{
    img->w[addr] = w;
    img->def[addr] = 1;
    if (addr >= img->n)
        img->n = (uint16_t)(addr + 1u);
}

void tops_put_code(tops_image_t *img, const chip_info_t *c, uint16_t addr, uint16_t count)
{
    /* GOTO words: never AND A / RET / MOV A / NOP */
    uint16_t base = c->code_bits == 13 ? 0x1800u : c->code_bits == 14 ? 0x3000u : 0x6000u;
    for (uint16_t i = 0; i < count; i++)
        tops_put(img, (uint16_t)(addr + i), (uint16_t)(base | ((addr + i) * 37u & 0x03FFu) | 1u));
}

void tops_put_calib(tops_image_t *img, const chip_info_t *c, uint16_t addr, uint8_t kind, uint32_t hz, uint16_t mV)
{
    calib_site_t s = {.addr = addr, .kind = kind, .target_hz = hz, .mV = mV};
    if (kind == CALIB_KIND_BG) {
        s.reg0 = c->reg_bgtr;
        s.reg1 = c->reg_gpcc;
        s.reg2 = c->reg_gpcs;
    } else {
        s.reg0 = kind == CALIB_KIND_IHRC ? c->reg_ihrcr : c->reg_ilrcr;
    }
    uint16_t w[CALIB_BG_LEN];
    uint8_t len = calib_placeholder_words(c->code_bits, &s, w);
    for (uint8_t i = 0; i < len; i++)
        tops_put(img, (uint16_t)(addr + i), w[i]);
}

void tops_put_serial(tops_image_t *img, const chip_info_t *c, uint16_t addr)
{
    static const char marker[] = "FPSERIAL";
    for (unsigned i = 0; i < SERIAL_SITE_LEN; i++)
        tops_put(img, (uint16_t)(addr + i), (uint16_t)(serial_ret_opcode(c->code_bits) | (uint8_t)marker[i]));
}

int tops_upload(const chip_info_t *c, const tops_image_t *img)
{
    static uint8_t pkg[TPK_MAX_SIZE];
    size_t len = tpk_build(pkg, c, img->w, img->def, img->n, img->flags, img->fuse);
    tp_frame_t rsp;
    tp_hello(tops_seq++);
    return tp_upload(pkg, len, &tops_seq, 256, &rsp);
}

const op_result_t *tops_run(uint8_t op_type, uint8_t op_flags, uint8_t source, uint16_t *start_status)
{
    op_request_t req = {.op_type = op_type, .op_flags = op_flags, .source = source};
    uint16_t id = 0;
    uint16_t st = op_manager_start(&req, &id);
    if (start_status)
        *start_status = st;
    if (st != PDK_OK)
        return NULL;
    pdk_core_poll();
    return op_manager_result(id);
}

static uint16_t img_get(void *ctx, uint16_t addr, bool *defined)
{
    const tops_image_t *img = ctx;
    *defined = addr < img->n && img->def[addr];
    return *defined ? img->w[addr] : 0xFFFF;
}

void tops_expected(const chip_info_t *c, const tops_image_t *img, const op_result_t *r, uint16_t *out)
{
    uint16_t mask = chip_word_mask(c);
    for (uint16_t a = 0; a < c->code_words; a++)
        out[a] = (a < img->n && img->def[a]) ? (uint16_t)(img->w[a] & mask) : mask;
    if (img->flags & 1u)   /* FUSE_PRESENT */
        out[c->code_words - 1u] = (uint16_t)(img->fuse & mask);
    calib_site_t sites[CALIB_MAX_SITES];
    uint8_t n = 0;
    uint16_t bad;
    calib_find_sites(c->code_bits, img_get, (void *)img, img->n, sites, &n, &bad);
    for (uint8_t i = 0; i < n; i++)
        calib_final_words(c->code_bits, &sites[i], r->calib[i].value, out + sites[i].addr);
    uint16_t serial[SERIAL_MAX_SITES];
    uint8_t ns = serial_find_sites(c->code_bits, img_get, (void *)img, img->n, serial, SERIAL_MAX_SITES);
    for (uint8_t i = 0; i < ns; i++)
        for (unsigned j = 0; j < SERIAL_SITE_LEN; j++)
            out[serial[i] + j] = (uint16_t)(serial_ret_opcode(c->code_bits) | r->serial[j]);
}
