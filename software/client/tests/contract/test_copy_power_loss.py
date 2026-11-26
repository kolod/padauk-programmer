# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-010 (T109): power lost at any point of COPY(MEASURE_MASTER) with two measured placeholders (the whole
operation is about 340 flash and chip operations, so the cuts also fall between the two measurements).
After the restart either MASTER_RESTORE_PENDING = 0 and the master is intact, or it is 1 and
RESTORE_MASTER — from the PC or started locally — brings the master back bit for bit."""
import random
import time

from conftest import iterations
from padauk_prog.protocol import (COPY_ALLOW_MODIFY_MASTER, COPY_MEASURE, Op, PkgState, ProgrammerError,
                                  ProtocolError, Status)
from padauk_prog.transport import TransportError

from ops_helpers import build, chip_memory, image, program

OPS = 350   # a little more than COPY(MEASURE_MASTER, MEASURE_MASTER) takes


def local_restore(sim, p, state):
    """The restore started with the programmer's button. The socket gets a blank chip of the same type (a
    chip present at power-up holds local starts back until a self-test): it receives the master's contents."""
    p.close()
    sim.restart("--chip", "none")
    sim.proc.stdin.write("swap-chip PFS154:offset=3\n")
    sim.proc.stdin.flush()
    p = sim.connect()
    before = p.op_result(0)
    before_id = before.op_id if before else 0
    sim.proc.stdin.write("local-start program\n")   # replaced by RESTORE_MASTER: a restore is pending
    sim.proc.stdin.flush()
    deadline = time.monotonic() + 20
    while True:   # None while it runs
        r = p.op_result(0)
        if r and r.op_id != before_id:
            break
        assert time.monotonic() < deadline
        time.sleep(0.05)
    assert r.op_type == Op.RESTORE_MASTER and r.source == 1
    return p, r


def test_power_loss_during_measure_master(pdk_sim, tmp_path):
    state = str(tmp_path / "master.bin")
    sim = pdk_sim("--chip", "PFS154:offset=3", "--chip-state", state)
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154", ilrc=True, bg=True)))
    assert program(p).status == Status.OK
    p.close()
    golden = chip_memory(state, "PFS154")

    rnd = random.Random(2025)
    outcomes = {"intact": 0, "pc": 0, "local": 0}
    for i in range(iterations(100, 20)):
        cut = rnd.randrange(0, OPS)
        sim.restart("--chip", "PFS154:offset=3", "--power-fail-after", str(cut))
        try:
            p = sim.connect()
            p.copy(0, COPY_MEASURE, COPY_MEASURE, True, COPY_ALLOW_MODIFY_MASTER)
            p.close()
        except (ProgrammerError, ProtocolError, TransportError):
            pass
        sim.restart("--chip", "PFS154:offset=3")
        p = sim.connect()
        st = p.get_status()
        assert st.pkg_state == PkgState.VALID, (i, cut)
        if not st.flags & 1:
            assert chip_memory(state, "PFS154") == golden, (i, cut)
            outcomes["intact"] += 1
        else:
            if outcomes["pc"] <= outcomes["local"]:
                r = p.restore_master()
                outcomes["pc"] += 1
            else:
                p, r = local_restore(sim, p, state)
                outcomes["local"] += 1
            assert r.status == Status.OK, (i, cut, r.status)
            assert chip_memory(state, "PFS154") == golden, (i, cut)
            assert p.get_status().flags & 1 == 0
        p.close()
    assert outcomes["intact"] and outcomes["pc"] and outcomes["local"], outcomes
