# SPDX-License-Identifier: GPL-3.0-or-later
"""PROGRAM end to end on pdk-sim's chip model (T070): stages, calibration, random serial number, checksums,
chip content; wrong chip; incompatible OTP chip; `pdkprog write` from an Intel HEX file."""
from padauk_prog import cli, ihex
from padauk_prog.protocol import Op, Stage, Status

from ops_helpers import (CHIPS, IHRC_HZ, build, chip_memory, expected_memory, image, program,
                         write_chip_memory)


def test_program_flash_chip(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "PFS154:offset=3", "--chip-state", state)
    p = sim.connect()
    words = image("PFS154")
    p.upload_package(build("PFS154", words))
    seen = []
    r = program(p, on_progress=lambda pr: seen.append((pr.stage, pr.stage_count)))
    assert r.status == Status.OK, r
    stages = [s for i, (s, _) in enumerate(seen) if i == 0 or seen[i - 1][0] != s]
    assert stages == [Stage.PKG_CHECK, Stage.ID, Stage.ERASE, Stage.BLANK_CHECK, Stage.WRITE, Stage.VERIFY,
                      Stage.CALIBRATE, Stage.PKG_CHECK, Stage.FUSE, Stage.FINAL_VERIFY]
    assert seen[-1][1] == 10
    assert len(r.calib) == 1 and abs(r.calib[0].measured_hz - IHRC_HZ) <= IHRC_HZ // 10
    assert r.serial_valid and r.serial != 0
    assert r.image_crc_raw and r.image_crc_norm
    assert r.fuse_written == CHIPS["PFS154"]["fuse"]
    assert chip_memory(state, "PFS154") == expected_memory("PFS154", words, r)

    first = r                                    # the next chip of the batch: another serial, same norm
    write_chip_memory(state, [0x3FFF] * 0x800)   # (FLASH: PROGRAM erases anyway)
    r = program(p)
    assert r.status == Status.OK
    assert r.serial != first.serial
    assert r.image_crc_norm == first.image_crc_norm
    p.close()


def test_wrong_chip(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "PMS150C", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    r = program(p)
    assert r.status == Status.CHIP_ID_MISMATCH and r.stage == Stage.ID
    assert r.chip_id_found == CHIPS["PMS150C"]["id"]
    p.close()


def test_incompatible_otp_chip_is_not_touched(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    mem = [0x1FFF] * 0x400
    mem[0x10] = 0x0000   # written by something else
    write_chip_memory(state, mem)
    sim = pdk_sim("--chip", "PMS150C", "--chip-state", state)
    p = sim.connect()
    p.upload_package(build("PMS150C", image("PMS150C")))
    r = program(p)
    assert r.status == Status.CHIP_NOT_BLANK and r.stage == Stage.BLANK_CHECK
    assert r.fail_addr == 0x10 and r.actual == 0
    assert chip_memory(state, "PMS150C") == mem
    p.close()


def test_cli_write_from_hex(pdk_sim, tmp_path, capsys):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "PFS154", "--chip-state", state)
    words = image("PFS154")
    hexfile = tmp_path / "fw.hex"
    hexfile.write_text(ihex.format_words(words))
    assert cli.main(["-p", sim.url, "write", str(hexfile), "-n", "PFS154", "--fuse", "0x3FFD", "-q"]) == 0
    out = capsys.readouterr().out
    assert "Programming done" in out and "Serial number: 0x" in out and "Calibration IHRC" in out
    assert cli.main(["-p", sim.url, "verify", "-q"]) == 0
    assert "Verify OK" in capsys.readouterr().out
    assert cli.main(["-p", sim.url, "probe", "-q"]) == 0
    assert "PFS154" in capsys.readouterr().out
    assert cli.main(["-p", sim.url, "status"]) == 0
    out = capsys.readouterr().out
    assert "2 ok" not in out and "1 ok" in out   # one PROGRAM so far
    assert cli.main(["-p", sim.url, "erase", "-q"]) == 0
    assert cli.main(["-p", sim.url, "blankcheck", "-n", "PFS154", "-q"]) == 0
    assert cli.main(["-p", sim.url, "verify", "-q"]) == 1
    assert "VERIFY_FAILED" in capsys.readouterr().err
    p = sim.connect()
    assert p.run_op(Op.PROBE).chip_id_found == CHIPS["PFS154"]["id"]
    p.close()
