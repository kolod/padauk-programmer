# SPDX-License-Identifier: GPL-3.0-or-later
"""Supply calibration with an external meter, end to end through pdkprog against pdk-sim.

The simulator gets scale errors like the G474 board (unbuffered DAC with a pull-down, 1 % dividers);
the "meter" reading is the set point times the output error, which the test knows.
"""
from padauk_prog import cli
from padauk_prog.protocol import ProgrammerError, Status

OUT = {"vdd": 0.945, "vpp": 0.948}
MEAS = {"vdd": 1.012, "vpp": 0.988}
SCALE = [arg for r in ("vdd", "vpp") for arg in ("--power-scale", f"{r}:{OUT[r]}:{MEAS[r]}")]


def meter(set_mV: int, rail: str) -> str:
    return f"{set_mV * OUT[rail] / 1000:.3f}"


def cal_points(p):
    s = p.cal_start()
    p.cal_confirm(False)
    return s["vdd_set_mV"], s["vpp_set_mV"]


def test_calibrate_accept_and_keep(pdk_sim, capsys):
    sim = pdk_sim(*SCALE)
    p = sim.connect()
    assert p.cal_get()["valid"] is False
    vdd_set, vpp_set = cal_points(p)
    p.close()

    args = ["-p", sim.url, "calibrate", "--yes", "--vdd", meter(vdd_set, "vdd"), "--vpp", meter(vpp_set, "vpp"),
            "--accept"]
    assert cli.main(args) == 0
    out = capsys.readouterr().out
    assert "VDD = 5.947 V" in out and "VPP = 12.000 V" in out   # sim limits 6.26 / 13.27 V
    assert "Calibration stored." in out

    sim.restart(*SCALE)
    p = sim.connect()
    c = p.cal_get()
    assert c["valid"]
    meas_vdd, meas_vpp, out_vdd, out_vpp = c["cal_permille"]
    assert abs(out_vdd - (-55)) <= 1 and abs(out_vpp - (-52)) <= 1
    assert abs(meas_vdd - (-12)) <= 1 and abs(meas_vpp - 12) <= 1
    p.close()


def test_reject_keeps_uncalibrated(pdk_sim, capsys):
    sim = pdk_sim(*SCALE)
    p = sim.connect()
    vdd_set, vpp_set = cal_points(p)
    p.close()
    args = ["-p", sim.url, "calibrate", "--yes", "--vdd", meter(vdd_set, "vdd"), "--vpp", meter(vpp_set, "vpp"),
            "--reject"]
    assert cli.main(args) == 1
    assert "discarded" in capsys.readouterr().out
    p = sim.connect()
    assert p.cal_get()["valid"] is False
    p.close()


def test_typo_is_refused(pdk_sim, capsys):
    sim = pdk_sim(*SCALE)
    args = ["-p", sim.url, "calibrate", "--yes", "--vdd", "5.3", "--vpp", "1.2", "--accept"]
    assert cli.main(args) == 1
    assert "BAD_PAYLOAD" in capsys.readouterr().err
    p = sim.connect()
    c = p.cal_get()
    assert c["valid"] is False and c["state"] == 0   # voltages off, nothing stored
    p.close()


def test_show_and_reset(pdk_sim, capsys):
    sim = pdk_sim(*SCALE)
    p = sim.connect()
    vdd_set, vpp_set = cal_points(p)
    p.close()
    cli.main(["-p", sim.url, "calibrate", "--yes", "--vdd", meter(vdd_set, "vdd"), "--vpp", meter(vpp_set, "vpp"),
              "--accept"])
    capsys.readouterr()
    assert cli.main(["-p", sim.url, "calibrate", "--show"]) == 0
    assert "VPP output" in capsys.readouterr().out
    assert cli.main(["-p", sim.url, "calibrate", "--reset"]) == 0
    assert cli.main(["-p", sim.url, "calibrate", "--show"]) == 0
    assert "not calibrated" in capsys.readouterr().out


def test_session_loss_drops_voltages(pdk_sim):
    sim = pdk_sim(*SCALE)
    p = sim.connect()
    p.cal_start()
    p.close()   # host disappears while the supplies are raised
    p = sim.connect()
    assert p.cal_get()["state"] == 0
    try:
        p.cal_set(5000, 11000)
    except ProgrammerError as e:
        assert e.status == Status.NOT_ALLOWED_NOW
    else:
        raise AssertionError("calibration survived the session loss")
    p.close()
