/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Effective image: what is written into / expected from the chip for a stored package
 * (contracts/package-format.md, "Ефективний образ"; data-model EffectiveImagePlan). Computed word by
 * word from the package and the plan of PKG_COMMIT, no RAM copy of the image. */
#ifndef PDK_EFFECTIVE_IMAGE_H
#define PDK_EFFECTIVE_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "header.h"
#include "validate.h"

#define EFF_SERIAL_BYTES 8u

typedef enum {
    EFF_PLAIN = 0,    /* package word (or NOP from SECURE_FILL) */
    EFF_UNWRITTEN,    /* not written: stays blank (all ones) */
    EFF_RESERVED,     /* reserved area of the chip: never written or compared */
    EFF_FUSE,         /* last word: written last, apart from the main write */
    EFF_CALIB,        /* calibration site `index`, word `offset` */
    EFF_SERIAL,       /* serial number site `index`, byte `offset` */
} eff_kind_t;

typedef struct {
    eff_kind_t kind;
    uint8_t index;
    uint8_t offset;
} eff_class_t;

typedef struct {
    const chip_info_t *chip;
    const pkg_plan_t *plan;
    word_get_fn get;
    void *ctx;
    bool secure_fill;
    uint16_t fill_end;
    uint16_t final_mask;                   /* bit i: site i is in finished form */
    uint8_t value[CALIB_MAX_SITES];        /* finished: k; loop: its MOV A,#… immediate */
    uint8_t serial[EFF_SERIAL_BYTES];      /* little-endian, zero until eff_set_serial() */
} eff_image_t;

/* All calibration sites start as measurement loops (CALIB_LOOP_IMM), serial number 0. */
void eff_init(eff_image_t *e, const chip_info_t *chip, const pkg_header_t *h, const pkg_plan_t *plan,
              word_get_fn get, void *ctx);

void eff_set_loop(eff_image_t *e, uint8_t site, uint8_t imm);
void eff_set_final(eff_image_t *e, uint8_t site, uint8_t k);
void eff_set_serial(eff_image_t *e, const uint8_t serial[EFF_SERIAL_BYTES]);

eff_class_t eff_classify(const eff_image_t *e, uint16_t addr);

/* Word for `addr` (masked to the core width): blank (all ones) for EFF_UNWRITTEN / EFF_RESERVED and an
 * undefined fuse. */
uint16_t eff_word(const eff_image_t *e, uint16_t addr);

#endif
