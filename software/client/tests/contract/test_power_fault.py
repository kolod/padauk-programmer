# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-011 (T076): a supply that does not settle (slow, stuck, low) → VPP_FAULT / VDD_FAULT at once, RESULT
names the rail with the set and the last measured voltage, the chip gets nothing."""
import os

import pytest

from padauk_prog.protocol import Stage, Status

from ops_helpers import build, image, program

CASES = [("vpp:slow", Status.VPP_FAULT, 1, 4500), ("vpp:stuck", Status.VPP_FAULT, 1, 4500),
         ("vdd:low", Status.VDD_FAULT, 0, 2000)]


@pytest.mark.parametrize("fault,status,rail,set_mV", CASES)
def test_supply_fault(pdk_sim, tmp_path, fault, status, rail, set_mV):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "PFS154", "--chip-state", state, "--power-fault", fault)
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    r = program(p)
    p.close()
    assert r.status == status and r.stage == Stage.ID          # the first voltage of the operation
    assert r.v_rail == rail and r.v_set_mV == set_mV
    assert abs(r.v_meas_mV - set_mV) > max(set_mV // 50, 30)    # outside the tolerance
    assert r.elapsed_ms < 1000
    assert not os.path.exists(state)                            # no write, no erase
