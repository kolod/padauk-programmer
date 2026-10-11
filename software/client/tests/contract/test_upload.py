# SPDX-License-Identifier: GPL-3.0-or-later
"""US1: package upload against pdk-sim (T044)."""
import os

import pytest

from padauk_prog import cli, ihex, package
from padauk_prog.protocol import Cmd, PkgState, ProgrammerError, Status

from pkg_helpers import manifest, u16, vector


def test_upload_info_read_and_restart(pdk_sim):
    sim = pdk_sim()
    for e in manifest():
        if e["stage"] != "ok":
            continue
        pkg = vector(e)
        p = sim.connect()
        res = p.upload_package(pkg)
        assert res["crc32"] == e["crc32"], e["name"]
        assert (res["calib_sites"], res["serial_sites"], res["effective_words"]) == \
            (e["calib_sites"], e["serial_sites"], e["effective_words"]), e["name"]
        info = p.package_info()
        assert info["state"] == PkgState.VALID
        assert info["crc32"] == e["crc32"]
        assert info["header"] == pkg[:128]
        assert p.package_read() == pkg
        p.close()

    last = vector([e for e in manifest() if e["stage"] == "ok"][-1])
    sim.restart()
    p = sim.connect()
    assert p.package_info()["state"] == PkgState.VALID
    assert p.package_read() == last
    assert p.package_check()["crc32"] == package.trailer_crc(last)
    p.close()


def test_every_invalid_vector_gives_exact_code(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    good = vector("ok_pfs154_ihrc")
    p.upload_package(good)
    for e in manifest():
        if e["stage"] == "ok":
            continue
        with pytest.raises(ProgrammerError) as exc:
            p.upload_package(vector(e))
        err = exc.value
        assert err.status == e["status"], e["name"]
        if e["stage"] == "begin":
            assert err.command == Cmd.PKG_BEGIN, e["name"]
        else:
            assert err.command == Cmd.PKG_COMMIT, e["name"]
            assert u16(err.payload) == e["addr"], e["name"]
        info = p.package_info()   # the active package is untouched
        assert info["state"] == PkgState.VALID and info["header"] == good[:128], e["name"]
    assert p.package_read() == good
    p.close()


def test_delete_and_empty_store(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    with pytest.raises(ProgrammerError) as exc:
        p.package_read()
    assert exc.value.status == Status.STORE_EMPTY
    p.upload_package(vector("ok_pfs154_3words"))
    p.package_delete()
    assert p.package_info()["state"] == PkgState.EMPTY
    p.close()
    sim.restart()
    p = sim.connect()
    assert p.package_info()["state"] == PkgState.EMPTY
    p.close()


def _write_ihx(tmp_path, name, words):
    path = tmp_path / name
    path.write_text(ihex.format_words(words))
    return str(path)


def test_cli_upload_warns_without_calibration(pdk_sim, tmp_path, capsys):
    sim = pdk_sim()
    plain = _write_ihx(tmp_path, "plain.ihx", {0: 0x0070, 1: 0x0000, 2: 0x3000})
    assert cli.main(["-p", sim.url, "upload", plain, "-n", "PFS154"]) == 0
    out = capsys.readouterr()
    assert "calibration sites 0" in out.out
    assert "warning: no calibration placeholder" in out.err

    hello = os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests", "vectors", "examples",
                         "helloworld_pfs154.ihx")
    assert cli.main(["-p", sim.url, "upload", hello, "-n", "PFS154"]) == 0
    out = capsys.readouterr()
    assert "calibration sites 1" in out.out and "warning" not in out.err


def test_cli_refusals(pdk_sim, tmp_path, capsys):
    sim = pdk_sim()
    f = _write_ihx(tmp_path, "x.ihx", {0: 0})
    assert cli.main(["-p", sim.url, "upload", f, "-n", "PFS154", "--nocalibrate"]) == 1
    assert "EASY_PDK_CALIBRATE" in capsys.readouterr().err
    assert cli.main(["-p", sim.url, "upload", f, "-n", "PFS154", "--serial", "5"]) == 1
    assert "random" in capsys.readouterr().err
    assert cli.main(["-p", sim.url, "upload", f, "-n", "PMS150C", "--noblankchk"]) == 1
    assert "OTP" in capsys.readouterr().err
    assert cli.main(["-p", sim.url, "upload", f, "-n", "PFS154", "--noblankchk"]) == 0   # FLASH: allowed
    capsys.readouterr()
    reserved = _write_ihx(tmp_path, "r.ihx", {0x7E0: 0})
    assert cli.main(["-p", sim.url, "upload", reserved, "-n", "PFS154"]) == 1
    err = capsys.readouterr().err
    assert "PKG_RESERVED_AREA" in err and "0x07E0" in err
