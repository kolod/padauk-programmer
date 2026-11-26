# SPDX-License-Identifier: GPL-3.0-or-later
"""RUN with the UART bridge (T119, US6). The chip model in pdk-sim answers 'U' + "hello\n" and echoes:
OP_START(RUN, 5000) → EVT_UART_LINK and EVT_UART_RX; UART_TX reaches the model; UART_TX outside RUN →
NOT_ALLOWED_NOW; OP_CANCEL → OK with the voltage removed; a lost session → HOST_DISCONNECTED, voltage
removed. pdkprog start shows "Connected @… baud" and the text."""
import struct
import time

import pytest

from padauk_prog import cli
from padauk_prog.protocol import Evt, Op, ProgrammerError, Stage, Status


def collect(p, until, timeout=5.0):
    """UART events until `until(rx_bytes, link_baud)` holds."""
    rx, baud = b"", None
    deadline = time.monotonic() + timeout
    while not until(rx, baud):
        assert time.monotonic() < deadline, (rx, baud)
        for e in p.poll_events(0.05):
            if e.code == Evt.UART_RX:
                rx += e.payload
            elif e.code == Evt.UART_LINK:
                baud = struct.unpack_from("<I", e.payload)[0]
    return rx, baud


def vdd_off(p) -> bool:
    s = p.supply_get()
    return s["vdd_set_mV"] == 0 and s["vdd_meas_mV"] < 100


def test_run_bridge_and_cancel(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    with pytest.raises(ProgrammerError) as e:
        p.uart_tx(b"x")
    assert e.value.status == Status.NOT_ALLOWED_NOW

    op_id = p.run_start(5000)
    rx, baud = collect(p, lambda rx, baud: baud and rx == b"hello\n")
    assert baud == 115200
    st = p.get_status()
    assert st.state == 3 and st.op_type == Op.RUN and st.op_stage == Stage.RUN
    p.uart_tx(b"ping")
    collect(p, lambda rx, baud: rx == b"ping")   # the model's echo
    with open(sim.flash + ".log") as f:
        assert "uart tx: ping" in f.read()       # it reached the chip

    p.cancel_op(op_id)
    r = p.wait_op(op_id, timeout=5)
    assert r.op_type == Op.RUN and r.status == Status.OK and r.stage == Stage.RUN
    assert p.get_status().state == 0 and vdd_off(p)
    with pytest.raises(ProgrammerError) as e:
        p.uart_tx(b"x")
    assert e.value.status == Status.NOT_ALLOWED_NOW
    p.close()


def test_session_loss_stops_run(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    op_id = p.run_start(3300)
    collect(p, lambda rx, baud: baud)
    p.close()   # the PC is gone
    time.sleep(0.3)
    p = sim.connect()
    deadline = time.monotonic() + 5
    while (r := p.op_result(0)) is None or r.op_id != op_id:
        assert time.monotonic() < deadline
        time.sleep(0.05)
    assert r.status == Status.HOST_DISCONNECTED
    assert p.get_status().state == 0 and vdd_off(p)
    p.close()


def test_pdkprog_start(pdk_sim, capsys):
    sim = pdk_sim()
    assert cli.main(["-p", sim.url, "start", "-r", "3.3", "--seconds", "0.5"]) == 0
    captured = capsys.readouterr()
    assert "Connected @115200 baud" in captured.err
    assert captured.out == "hello\n"
    assert "switched off" in captured.err
