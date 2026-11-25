# SPDX-License-Identifier: GPL-3.0-or-later
"""SELF_TEST (T095): an empty socket → OK with every level; a chip in it → SELFTEST_CHIP_PRESENT without high
voltage; a stuck VPP → SELFTEST_FAILED at that level; the start-up run shows in STATUS.selftest_state."""
import pytest

from padauk_prog import cli
from padauk_prog.protocol import Op, Status

CASES = [((), Status.OK, 2), (("--chip", "PFS154"), Status.SELFTEST_CHIP_PRESENT, 4),
         (("--power-fault", "vpp:stuck"), Status.SELFTEST_FAILED, 3)]


@pytest.mark.parametrize("args,status,state", CASES)
def test_selftest(pdk_sim, args, status, state):
    sim = pdk_sim(*args)
    p = sim.connect()
    s = p.get_status()
    assert s.selftest_state == state                 # the start-up run (source = local)
    assert bool(s.flags & 2) == (state != 2)         # LOCAL_START_BLOCKED unless it passed
    start = p.op_result(0)
    assert start.op_type == Op.SELF_TEST and start.source == 1 and start.status == status
    r = p.run_op(Op.SELF_TEST)
    assert r.status == status and r.source == 0
    if status == Status.OK:
        vdd = [lv for lv in r.levels if lv.rail == 0]
        vpp = [lv for lv in r.levels if lv.rail == 1]
        assert vdd[-1].set_mV == 0 and vpp[-1].set_mV == 0
        assert [lv.set_mV for lv in vdd[:-1]] == sorted({lv.set_mV for lv in vdd[:-1]})
        assert all(lv.ok for lv in r.levels) and r.levels.index(vpp[0]) == len(vdd)
    elif status == Status.SELFTEST_CHIP_PRESENT:
        assert r.levels == [] and r.chip_id_found == 0xAA1
    else:
        assert r.levels[-1].rail == 1 and not r.levels[-1].ok and r.v_rail == 1
    p.close()


def test_cli_selftest(pdk_sim, capsys):
    sim = pdk_sim()
    assert cli.main(["-p", sim.url, "selftest", "--yes", "-q"]) == 0
    out = capsys.readouterr().out
    assert "Self-test passed" in out and "VPP" in out and "settle ms" in out
    assert cli.main(["-p", sim.url, "info"]) == 0
    out = capsys.readouterr().out
    assert "Self-test: passed" in out and "self-test" in out and "patch" in out
