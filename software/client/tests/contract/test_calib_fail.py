# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-013 (T075): a chip whose oscillator cannot reach the target → CALIB_FAILED, nothing written after the
measurement loop; with CALIB_FINALIZE_ON_FAIL → finished with the best value (CALIB_FORCED); once the cause
is gone, PROGRAM again completes the same OTP chip (OK, RESUMED)."""
from padauk_prog import cli
from padauk_prog.protocol import OPF_CALIB_FINALIZE_ON_FAIL, Stage, Status, WARN_CALIB_FORCED, WARN_RESUMED

from ops_helpers import CHIPS, IHRC_SITE, build, chip_memory, expected_memory, image, program, ref

CHIP = "PMS150C"


def site_form(mem):
    m = ref.match_at(CHIP, mem, IHRC_SITE)
    return m[0] if m else None


def test_failed_calibration_and_repair(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    words = image(CHIP)
    sim = pdk_sim("--chip", CHIP + ":osc=broken", "--chip-state", state)
    p = sim.connect()
    p.upload_package(build(CHIP, words))
    r = program(p)
    p.close()
    assert r.status == Status.CALIB_FAILED and r.stage == Stage.CALIBRATE
    assert r.calib[0].site_status == 1 and r.fuse_written == 0xFFFF
    mem = chip_memory(state, CHIP)
    assert site_form(mem) == ref.LOOP                                   # left for a later retry
    assert mem[CHIPS[CHIP]["fuse_addr"]] == (1 << CHIPS[CHIP]["bits"]) - 1

    sim.restart("--chip", CHIP, "--chip-state", state)   # the board is fixed
    p = sim.connect()
    r = program(p)
    p.close()
    assert r.status == Status.OK and r.warnings & WARN_RESUMED
    assert chip_memory(state, CHIP) == expected_memory(CHIP, words, r)


def test_finalize_on_fail(pdk_sim, tmp_path, capsys):
    state = str(tmp_path / "chip.bin")
    words = image(CHIP)
    sim = pdk_sim("--chip", CHIP + ":osc=broken", "--chip-state", state)
    p = sim.connect()
    p.upload_package(build(CHIP, words))
    r = program(p, OPF_CALIB_FINALIZE_ON_FAIL)
    p.close()
    assert r.status == Status.CALIB_FAILED and r.stage == Stage.FINAL_VERIFY
    assert r.warnings & WARN_CALIB_FORCED and r.calib[0].site_status == 2
    assert r.fuse_written == CHIPS[CHIP]["fuse"]
    assert chip_memory(state, CHIP) == expected_memory(CHIP, words, r)

    sim.restart("--chip", CHIP + ":osc=broken", "--chip-state", str(tmp_path / "chip2.bin"))
    assert cli.main(["-p", sim.url, "program", "-q"]) == 1
    err = capsys.readouterr().err
    assert "CALIB_FAILED" in err and "measurement loop" in err
