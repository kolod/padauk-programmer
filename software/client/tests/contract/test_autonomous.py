# SPDX-License-Identifier: GPL-3.0-or-later
"""Autonomous programming (T100): local starts through pdk-sim's stdin instead of a button."""
import os
import time

from padauk_prog import cli
from padauk_prog.protocol import EVT_OP_DONE, EVT_OP_STARTED, Op, Result, Stage, Status

from ops_helpers import build, image


def press(sim, command: str) -> None:
    sim.proc.stdin.write(command + "\n")
    sim.proc.stdin.flush()


def local_result(p, after_id: int, timeout: float = 30.0) -> Result:
    """The next RESULT of a local operation started after `after_id`."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = p.op_result(0)
        if r is not None and r.op_id != after_id:
            assert r.source == 1
            return r
        time.sleep(0.02)
    raise AssertionError("no local operation finished")


def insert_chip(sim, spec: str = "PFS154") -> None:
    """The socket was empty at power-on (so the start-up self-test passed); now a chip goes in."""
    press(sim, f"swap-chip {spec}")
    time.sleep(0.05)


def last_id(p) -> int:
    r = p.op_result(0)
    return r.op_id if r else 0


def ui_lines(sim) -> list:
    with open(sim.flash + ".log") as f:
        return [line.split()[1:] for line in f if line.startswith("ui_state")]


def test_three_chips_three_serials_and_stats_survive(pdk_sim, tmp_path):
    flash = str(tmp_path / "flash.bin")
    sim = pdk_sim("--chip", "none", "--chip-state", str(tmp_path / "chip.bin"), flash=flash)
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    insert_chip(sim)
    serials = set()
    for _ in range(3):
        before = last_id(p)
        p.poll_events(0)   # drop what came before
        press(sim, "local-start program")
        r = local_result(p, before)
        assert r.op_type == Op.PROGRAM and r.status == Status.OK
        serials.add(r.serial)
        events = [(e.code, e.payload) for e in p.poll_events(0.1)]
        assert any(c == EVT_OP_STARTED and pl[3] == 1 for c, pl in events)   # source = local
        assert any(c == EVT_OP_DONE for c, _ in events)
    assert len(serials) == 3
    states = [line[0] for line in ui_lines(sim)]
    assert "BUSY" in states and states[-1] == "SUCCESS"
    assert any(line[0] == "BUSY" and line[1] == f"stage={Stage.WRITE}" for line in ui_lines(sim))
    p.close()

    sim.restart("--chip", "none")   # power cycle: the statistics are in the journal
    p = sim.connect()
    assert p.stats_get() == (3, 0)
    p.close()


def test_broken_package_is_not_written(pdk_sim, tmp_path):
    flash = str(tmp_path / "flash.bin")
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "none", "--chip-state", state, flash=flash)
    p = sim.connect()
    pkg = build("PFS154", image("PFS154"))
    p.upload_package(pkg)
    p.close()
    sim.stop()
    with open(flash, "r+b") as f:   # damage one byte of the stored image
        mem = bytearray(f.read())
        at = mem.find(pkg[:16]) + 300
        mem[at] ^= 0x04
        f.seek(0)
        f.write(mem)
    sim.start()
    insert_chip(sim)
    p = sim.connect()
    before = last_id(p)
    press(sim, "local-start program")
    r = local_result(p, before)
    assert r.status == Status.STORE_CORRUPTED and r.stage == Stage.PKG_CHECK
    assert not os.path.exists(state)   # no voltage, no write
    assert ui_lines(sim)[-1][0] == "FAIL" and "status=0x0302" in ui_lines(sim)[-1]
    p.close()


def test_failed_self_test_blocks_local_programming(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "none", "--power-fault", "vpp:stuck")
    p = sim.connect()
    s = p.get_status()
    assert s.flags & 2 and s.selftest_state == 3   # LOCAL_START_BLOCKED
    before = last_id(p)
    press(sim, "local-start program")
    r = local_result(p, before)
    assert r.op_type == Op.SELF_TEST and r.status == Status.SELFTEST_FAILED
    p.close()


def test_verify_selftest_and_cancel(pdk_sim, tmp_path, capsys):
    sim = pdk_sim("--chip", "none", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    insert_chip(sim, "PFS154:slow=3000")
    before = last_id(p)
    press(sim, "local-start program")
    deadline = time.monotonic() + 10
    while p.get_status().op_stage != Stage.WRITE:
        assert time.monotonic() < deadline
        time.sleep(0.02)
    press(sim, "local-cancel")
    r = local_result(p, before)
    assert r.status == Status.CANCELLED and r.stage == Stage.WRITE
    s = p.supply_get()
    assert s["vpp_meas_mV"] < 100 and s["vdd_meas_mV"] < 100

    press(sim, "local-start program")   # this time to the end, then the other local actions
    before = r.op_id
    r = local_result(p, before)
    assert r.status == Status.OK
    press(sim, "local-start verify")
    v = local_result(p, r.op_id)
    assert v.op_type == Op.VERIFY and v.status == Status.OK and v.serial == r.serial
    press(sim, "swap-chip none")
    press(sim, "local-start selftest")
    t = local_result(p, v.op_id)
    assert t.op_type == Op.SELF_TEST and t.status == Status.OK
    p.close()

    assert cli.main(["-p", sim.url, "stats"]) == 0
    assert "1 ok, 1 failed" in capsys.readouterr().out   # the cancelled chip counts as failed
    assert cli.main(["-p", sim.url, "stats", "reset"]) == 0
    capsys.readouterr()
    assert cli.main(["-p", sim.url, "stats"]) == 0
    assert "0 ok, 0 failed" in capsys.readouterr().out


def test_cli_monitor_shows_local_operations(pdk_sim, tmp_path, capsys):
    import threading
    sim = pdk_sim("--chip", "none", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    p.close()
    insert_chip(sim)
    threading.Timer(1.0, press, (sim, "local-start program")).start()
    assert cli.main(["-p", sim.url, "monitor", "--count", "1", "--timeout", "20"]) == 0
    out = capsys.readouterr().out
    assert "PROGRAM started (local)" in out and "PROGRAM: OK at FINAL_VERIFY, serial 0x" in out
