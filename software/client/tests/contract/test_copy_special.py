# SPDX-License-Identifier: GPL-3.0-or-later
"""Special masters (T110): a master whose calibration never finished (measurement loop) is refused by a
local COPY and by a COPY from the PC without permission, the stored package unchanged; with
ALLOW_MASTER_LOOP and a target from the PC the placeholder gets that target (FR-051). A master that writes
a constant into IHRCR outside a footprint and has a free-pdk serial number is copied as it is with
CALIB_UNRECOGNIZED and SERIAL_DUPLICATED (FR-052, FR-053)."""
import builtins
import time

from padauk_prog import cli
from padauk_prog.protocol import Op, Status

from ops_helpers import CHIPS, IHRC_SITE, build, image, program, ref

ID = CHIPS["PFS154"]["id"]


def uncalibrated_master(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "none", "--chip-state", str(tmp_path / "master.bin"))
    sim.proc.stdin.write("swap-chip PFS154:osc=broken\n")
    sim.proc.stdin.flush()
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    assert program(p).status == Status.CALIB_FAILED   # the loop stays in the master
    return sim, p


def test_master_with_a_measurement_loop(pdk_sim, tmp_path, capsys, monkeypatch):
    sim, p = uncalibrated_master(pdk_sim, tmp_path)
    crc = p.package_info()["crc32"]
    r = p.copy(ID)
    assert r.status == Status.COPY_MASTER_UNCALIBRATED and r.stage == 9   # READ
    assert r.fail_addr == IHRC_SITE and [c.site_status for c in r.calib] == [5]
    assert p.package_info()["crc32"] == crc

    before = p.op_result(0).op_id
    sim.proc.stdin.write("local-start copy\n")   # locally: refused the same way
    sim.proc.stdin.flush()
    deadline = time.monotonic() + 20
    while p.op_result(0).op_id == before:
        assert time.monotonic() < deadline
        time.sleep(0.05)
    r = p.op_result(0)
    assert r.source == 1 and r.op_type == Op.COPY and r.status == Status.COPY_MASTER_UNCALIBRATED
    assert p.package_info()["crc32"] == crc
    p.close()

    answers = iter(["t", "4000000"])   # pdkprog asks: cancel or give the target
    monkeypatch.setattr(builtins, "input", lambda prompt="": next(answers))
    assert cli.main(["-p", sim.url, "copy", "-q"]) == 0
    out = capsys.readouterr().out
    assert "measurement loop" in out and "target 4000000 Hz" in out
    p = sim.connect()
    h = p.package_read()
    words = [int.from_bytes(h[128 + 2 * a:130 + 2 * a], "little") for a in range(IHRC_SITE, IHRC_SITE + 10)]
    assert ref.match_at("PFS154", [0] * IHRC_SITE + words, IHRC_SITE) is None   # a placeholder now
    assert words[3] & 0xFF | (words[4] & 0xFF) << 8 | (words[5] & 0xFF) << 16 == 4000000 & 0xFFFFFF
    p.close()


def test_trim_write_and_serial_number(pdk_sim, tmp_path):
    words = image("PFS154", ihrc=False)
    words[0x20] = ref.MOV_A_IMM[14] | 0x55   # MOV A,#0x55 / MOV IO(IHRCR),A — not a footprint
    words[0x21] = ref.MOV_IO_A[14] | ref.CHIPS["PFS154"]["regs"]["ihrcr"]
    sim = pdk_sim("--chip", "none", "--chip-state", str(tmp_path / "master.bin"))
    sim.proc.stdin.write("swap-chip PFS154\n")
    sim.proc.stdin.flush()
    p = sim.connect()
    p.upload_package(build("PFS154", words))
    assert program(p).status == Status.OK
    r = p.copy(ID)
    assert r.status == Status.OK
    assert r.warnings & (1 << 6) and r.warnings & (1 << 5)   # CALIB_UNRECOGNIZED, SERIAL_DUPLICATED
    assert r.fail_addr == 0x20 and r.calib == []
    pkg = p.package_read()
    assert int.from_bytes(pkg[128 + 0x40:130 + 0x40], "little") == words[0x20]   # code as it is
    p.close()
