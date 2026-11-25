/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test helpers for operations: build an image with placeholders, upload it, run an operation on the
 * fake chip engine and compute what the chip must contain afterwards. */
#ifndef PDK_TEST_OPS_H
#define PDK_TEST_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "chips/chip_table.h"
#include "ops/op_manager.h"
#include "test_proto.h"

#define TOPS_MAX_WORDS 0x1000u

typedef struct {
    uint16_t w[TOPS_MAX_WORDS];
    uint8_t def[TOPS_MAX_WORDS];
    uint16_t n;
    uint32_t flags;
    uint16_t fuse;
} tops_image_t;

const chip_info_t *tops_chip(const char *name);

void tops_image_init(tops_image_t *img);
void tops_put(tops_image_t *img, uint16_t addr, uint16_t w);
/* Plain code words (no AND A / RET / MOV A patterns) at [addr, addr + count). */
void tops_put_code(tops_image_t *img, const chip_info_t *c, uint16_t addr, uint16_t count);
/* free-pdk placeholders: calibration (registers from the chip table) and serial number. */
void tops_put_calib(tops_image_t *img, const chip_info_t *c, uint16_t addr, uint8_t kind, uint32_t hz, uint16_t mV);
void tops_put_serial(tops_image_t *img, const chip_info_t *c, uint16_t addr);

/* HELLO + upload; returns TP_STAGE_OK on success. */
int tops_upload(const chip_info_t *c, const tops_image_t *img);

/* Starts an operation and runs it to the end; NULL if it could not start (*start_status). */
const op_result_t *tops_run(uint8_t op_type, uint8_t op_flags, uint8_t source, uint16_t *start_status);

/* The chip content after a successful PROGRAM: calibration values and serial number from `r`. */
void tops_expected(const chip_info_t *c, const tops_image_t *img, const op_result_t *r, uint16_t *out);

/* Next request sequence number (shared by the helpers and the tests). */
extern uint16_t tops_seq;

#endif
