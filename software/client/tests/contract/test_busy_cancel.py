# SPDX-License-Identifier: GPL-3.0-or-later
"""During PROGRAM (T071): PKG_BEGIN / OP_START → BUSY, GET_STATUS / OP_RESULT answer; OP_CANCEL → CANCELLED
in the stage it was in, supplies off."""
import time

from padauk_prog.protocol import Op, ProgrammerError, Stage, Status

from ops_helpers import build, image


def test_busy_and_cancel(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "PFS154:slow=3000", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    pkg = build("PFS154", image("PFS154"))
    p.upload_package(pkg)
    op_id = p.start_op(Op.PROGRAM)
    deadline = time.monotonic() + 10
    while p.get_status().op_stage != Stage.WRITE:   # GET_STATUS works during the operation
        assert time.monotonic() < deadline
        time.sleep(0.02)
    s = p.get_status()
    assert s.state == 2 and s.op_id == op_id and s.op_type == Op.PROGRAM
    for code, payload in ((0x0010, pkg[:128]), (0x0020, bytes([Op.PROBE, 0, 0, 0, 0, 0]))):
        try:
            p.call(code, payload)
        except ProgrammerError as e:
            assert e.status == Status.BUSY
        else:
            raise AssertionError(f"0x{code:04X} accepted during an operation")
    assert p.op_result(op_id) is None   # still running
    p.cancel_op(op_id)
    r = p.wait_op(op_id, timeout=10)
    assert r.status == Status.CANCELLED and r.stage == Stage.WRITE
    assert p.get_status().state == 0
    s = p.supply_get()
    assert s["vdd_set_mV"] == 0 and s["vpp_meas_mV"] < 100 and s["vdd_meas_mV"] < 100
    p.close()
