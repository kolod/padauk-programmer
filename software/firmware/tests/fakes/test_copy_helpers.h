/* SPDX-License-Identifier: GPL-3.0-or-later */
/* COPY requests for unit tests. */
#ifndef PDK_TEST_COPY_HELPERS_H
#define PDK_TEST_COPY_HELPERS_H

#include <string.h>

#include "chips/chip_table.h"
#include "ops/op_manager.h"
#include "proto/le.h"
#include "test_ops.h"

typedef struct {
    uint8_t mode_ihrc, mode_ilrc, bg_restore, flags;
    uint32_t target_ihrc, target_ilrc;
    uint16_t mV;
    const char *name;
} copy_ext_t;

/* COPY of the chip in the socket; NULL ext = none (KEEP_VALUE, KEEP_VALUE, BandGap kept). */
static inline const op_result_t *tops_copy(const chip_info_t *c, const copy_ext_t *e)
{
    static op_request_t req;
    memset(&req, 0, sizeof req);
    req.op_type = PDK_OP_COPY;
    req.chip_id12 = c->id12;
    if (e) {
        req.ext[0] = e->mode_ihrc;
        req.ext[1] = e->mode_ilrc;
        req.ext[2] = e->bg_restore;
        req.ext[3] = e->flags;
        put32(req.ext + 4, e->target_ihrc);
        put32(req.ext + 8, e->target_ilrc);
        put16(req.ext + 12, e->mV);
        req.ext_len = 14;
        if (e->name) {
            strncpy((char *)req.ext + 14, e->name, 32);
            req.ext_len = 46;
        }
    }
    return tops_run_req(&req, NULL);
}

#endif
