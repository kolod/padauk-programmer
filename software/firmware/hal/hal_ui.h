/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local controls and indication (contracts/chip-engine.md §4, FR-054).
 * The platform maps its buttons/switches/conveyor signals onto pdk_local_start()/pdk_local_cancel()
 * (provided by the core) and shows the state passed to hal_ui_state(). */
#ifndef PDK_HAL_UI_H
#define PDK_HAL_UI_H

#include <stdint.h>

typedef enum {
    PDK_LOCAL_PROGRAM = 0,
    PDK_LOCAL_COPY,
    PDK_LOCAL_VERIFY,
    PDK_LOCAL_SELF_TEST,
    PDK_LOCAL_RESTORE_MASTER,
} pdk_local_request_t;

typedef enum {
    PDK_UI_READY = 0,
    PDK_UI_BUSY,
    PDK_UI_SUCCESS,
    PDK_UI_FAIL,
    PDK_UI_BLOCKED,
} pdk_ui_state_t;

typedef enum {
    PDK_UI_FAIL_NONE = 0,
    PDK_UI_FAIL_STORAGE,
    PDK_UI_FAIL_NO_PACKAGE,
    PDK_UI_FAIL_CHIP,
    PDK_UI_FAIL_CALIB,
    PDK_UI_FAIL_POWER_SELFTEST,
    PDK_UI_FAIL_MASTER_PENDING,
} pdk_ui_fail_class_t;

typedef struct {
    uint8_t stage;               /* BUSY: current stage */
    uint8_t percent;             /* BUSY: overall percent */
    pdk_ui_fail_class_t fail;    /* FAIL: class */
    uint16_t status;             /* FAIL: status code */
} pdk_ui_detail_t;

/* Implemented by the platform. */
void hal_ui_state(pdk_ui_state_t state, const pdk_ui_detail_t *detail);

/* Implemented by the core, called by the platform. */
void pdk_local_start(pdk_local_request_t request);
void pdk_local_cancel(void);

#endif
