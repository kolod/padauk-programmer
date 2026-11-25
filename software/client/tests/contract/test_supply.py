# SPDX-License-Identifier: GPL-3.0-or-later
"""Arbitrary VDD/VPP (`pdkprog supply`, SUPPLY_SET / SUPPLY_GET) end to end against pdk-sim."""
import builtins
import re

from padauk_prog import cli
from padauk_prog.protocol import ProgrammerError, Status


def readings(out: str) -> list:
    """(rail, set V, read V) from every supplies line."""
    return [(m[0], float(m[1]), float(m[2])) for m in re.findall(r"(VDD|VPP) set\s+([\d.]+) V\s+reads\s+([\d.]+) V", out)]


def test_hold_applies_and_switches_off(pdk_sim, capsys):
    sim = pdk_sim()   # ideal supplies: no calibration needed
    assert cli.main(["-p", sim.url, "supply", "3.3", "12.5", "--hold", "1", "--yes"]) == 0
    out = capsys.readouterr().out
    rows = readings(out)
    assert len(rows) >= 4   # applied + at least one reading during the hold, both rails each
    for rail, set_v, read_v in rows:
        assert set_v == (3.3 if rail == "VDD" else 12.5)
        assert abs(read_v - set_v) <= 0.03
    assert "Supplies off." in out
    p = sim.connect()
    assert p.supply_get()["vdd_set_mV"] == 0
    p.close()


def test_interactive_levels(pdk_sim, capsys, monkeypatch):
    sim = pdk_sim()
    answers = iter(["y", "2.5", "5 12", "m", "6.5", "abc", "0", ""])
    monkeypatch.setattr(builtins, "input", lambda prompt="": next(answers))
    assert cli.main(["-p", sim.url, "supply"]) == 0
    out = capsys.readouterr().out
    sets = [(r, s) for r, s, _ in readings(out)]
    assert sets[:2] == [("VDD", 2.5), ("VPP", 0.0)]
    assert ("VDD", 5.0) in sets and ("VPP", 12.0) in sets
    assert "VDD is above the limit" in out          # 6.5 V > 6.26 V
    assert "Please enter one or two numbers" in out  # 'abc'
    assert sets[-2:] == [("VDD", 0.0), ("VPP", 0.0)]
    assert out.rstrip().endswith("Supplies off.")


def test_uncalibrated_board_reports_the_fault(pdk_sim, capsys):
    sim = pdk_sim("--power-scale", "vdd:0.945:1.012")   # output stage 5.5 % low, as on the G474 board
    assert cli.main(["-p", sim.url, "supply", "5", "--hold", "0", "--yes"]) == 1
    out = capsys.readouterr().out
    assert "VDD did not settle within tolerance: set 5.000 V" in out
    assert "not calibrated" in out


def test_no_check_keeps_the_rail_for_the_meter(pdk_sim, capsys):
    sim = pdk_sim("--power-scale", "vdd:0.945:1.000")
    assert cli.main(["-p", sim.url, "supply", "5", "--hold", "0", "--yes", "--no-check"]) == 0
    out = capsys.readouterr().out
    assert "Tolerance check off" in out
    (vdd,) = [r for r in readings(out) if r[0] == "VDD"][:1]
    assert vdd[1] == 5.0 and abs(vdd[2] - 4.725) <= 0.03   # 5.5 % low, and it stayed up
    assert re.search(r"VDD set .* spread\s+[\d.]+\.\.[\d.]+ V \(\d+ mV\)", out)


def test_session_loss_drops_voltages(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    s = p.supply_set(5000, 11000)
    assert s["vpp_set_mV"] == 11000 and abs(s["vpp_meas_mV"] - 11000) <= 30
    p.close()   # host disappears with the supplies up
    p = sim.connect()
    s = p.supply_get()
    assert s["vdd_set_mV"] == 0 and s["vpp_set_mV"] == 0 and s["vpp_meas_mV"] < 100
    p.close()


def test_calibration_takes_over(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    p.supply_set(3300, 0)
    p.cal_start()
    try:
        p.supply_set(3300, 0)
    except ProgrammerError as e:
        assert e.status == Status.NOT_ALLOWED_NOW
    else:
        raise AssertionError("SUPPLY_SET accepted during calibration")
    p.cal_confirm(False)
    assert p.supply_get()["vdd_set_mV"] == 0
    p.close()
