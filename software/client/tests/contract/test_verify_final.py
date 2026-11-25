# SPDX-License-Identifier: GPL-3.0-or-later
"""VERIFY and FINAL_VERIFY (T077): a calibrated chip with a serial number verifies with its own k and
number; an unfinished calibration does not; a weak cell is caught by FINAL_VERIFY; a package that reads back
wrong while the chip is written stops before the fuse (FR-049)."""
from padauk_prog.protocol import Op, Stage, Status

from ops_helpers import CHIPS, IHRC_SITE, build, chip_memory, image, program


def test_verify_programmed_and_unfinished(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "PFS154:offset=-4", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    r = program(p)
    assert r.status == Status.OK
    v = p.run_op(Op.VERIFY)
    assert v.status == Status.OK
    assert v.calib[0].value == r.calib[0].value and v.calib[0].site_status == 0
    assert v.serial_valid and v.serial == r.serial
    assert (v.image_crc_raw, v.image_crc_norm) == (r.image_crc_raw, r.image_crc_norm)
    p.close()

    sim.restart("--chip", "PFS154:osc=broken", "--chip-state", str(tmp_path / "broken.bin"))
    p = sim.connect()
    assert program(p).status == Status.CALIB_FAILED
    v = p.run_op(Op.VERIFY)
    assert v.status == Status.VERIFY_FAILED and v.calib[0].site_status == 1
    assert v.fail_addr == IHRC_SITE
    p.close()


def test_weak_cell_is_caught_by_final_verify(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", f"PMS150C:weak={IHRC_SITE + 2:#x}", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PMS150C", image("PMS150C")))
    r = program(p)
    p.close()
    assert r.status == Status.VERIFY_FAILED and r.stage == Stage.FINAL_VERIFY
    assert r.fail_addr == IHRC_SITE + 2
    assert r.actual & 0xFF == 0xFF and r.expected & 0xFF == r.calib[0].value


def test_package_read_wrong_while_writing(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "PMS150C", "--chip-state", state, "--corrupt-flash-during-write")
    p = sim.connect()
    p.upload_package(build("PMS150C", image("PMS150C")))
    r = program(p)
    p.close()
    assert r.status == Status.STORE_CORRUPTED and r.stage == Stage.WRITE
    assert r.fuse_written == 0xFFFF
    assert chip_memory(state, "PMS150C")[CHIPS["PMS150C"]["fuse_addr"]] == 0x1FFF
