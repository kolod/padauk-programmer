/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "indication.h"

#include "proto/proto_defs.h"

pdk_ui_fail_class_t indication_class(uint16_t status)
{
    switch (status) {
    case PDK_STORE_EMPTY: return PDK_UI_FAIL_NO_PACKAGE;
    case PDK_STORE_CORRUPTED:
    case PDK_STORE_WRITE_FAILED: return PDK_UI_FAIL_STORAGE;
    case PDK_CALIB_FAILED:
    case PDK_CALIB_WRONG_SITE: return PDK_UI_FAIL_CALIB;
    case PDK_VDD_FAULT:
    case PDK_VPP_FAULT:
    case PDK_SELFTEST_FAILED:
    case PDK_SELFTEST_CHIP_PRESENT: return PDK_UI_FAIL_POWER_SELFTEST;
    case PDK_MASTER_RESTORE_PENDING: return PDK_UI_FAIL_MASTER_PENDING;
    default: return (status & 0xFF00u) == 0x0200u ? PDK_UI_FAIL_STORAGE : PDK_UI_FAIL_CHIP;
    }
}

void indication_busy(uint8_t stage, uint8_t percent)
{
    pdk_ui_detail_t d = {.stage = stage, .percent = percent};
    hal_ui_state(PDK_UI_BUSY, &d);
}

void indication_fail(uint16_t status)
{
    pdk_ui_detail_t d = {.fail = indication_class(status), .status = status};
    hal_ui_state(PDK_UI_FAIL, &d);
}

void indication_done(uint8_t op_type, uint16_t status)
{
    pdk_ui_detail_t d = {0};
    if (op_type == PDK_OP_SELF_TEST) {
        if (status == PDK_OK)
            hal_ui_state(PDK_UI_READY, &d);      /* ready for local programming */
        else if (status == PDK_OP_UNSUPPORTED)
            hal_ui_state(PDK_UI_BLOCKED, &d);    /* cannot be checked here: local programming stays blocked */
        else
            indication_fail(status);
        return;
    }
    if (status == PDK_OK || status == PDK_CHIP_ALREADY_PROGRAMMED)
        hal_ui_state(PDK_UI_SUCCESS, &d);
    else
        indication_fail(status);
}
