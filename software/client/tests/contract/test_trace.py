# SPDX-License-Identifier: GPL-3.0-or-later
"""The diagnostic trace (protocol 1.1): TRACE_SET(1) → EVT_TRACE records of every supply step with its measured value
during an operation; off by default and after a session reset; a bad payload is refused; pdkprog --trace prints them."""
import pytest

from padauk_prog import cli
from padauk_prog.protocol import Op, ProgrammerError, Status


def test_trace_reports_the_supply_steps(pdk_sim):
    sim = pdk_sim("--chip", "PFS154")
    p = sim.connect()
    seen = []
    p.on_trace = seen.append
    p.run_op(Op.PROBE)
    assert seen == []                     # off by default
    p.trace_set(True)
    r = p.run_op(Op.PROBE)
    assert r.status == Status.OK
    power = [(x.b8, x.w1) for x in seen if x.kind == 1]
    assert (1, 4500) in power and (0, 2000) in power   # VPP 4.5 V and VDD 2.0 V of the probe
    vpp = next(x for x in seen if x.kind == 1 and x.b8 == 1)
    assert abs(vpp.w2 - 4500) <= 90 and "VPP 4500 mV: reached" in vpp.text()
    with pytest.raises(ProgrammerError) as e:
        p.call(0x0047, bytes([2]))
    assert e.value.status == Status.BAD_PAYLOAD
    p.close()

    p = sim.connect()                     # a new session: the trace is off again
    seen.clear()
    p.on_trace = seen.append
    p.run_op(Op.PROBE)
    assert seen == []
    p.close()


def test_pdkprog_trace(pdk_sim, capsys):
    sim = pdk_sim("--chip", "PFS154")
    assert cli.main(["-p", sim.url, "--trace", "probe"]) == 0
    err = capsys.readouterr().err
    assert "trace" in err and "VPP 4500 mV: reached" in err and "VDD 2000 mV: reached" in err
