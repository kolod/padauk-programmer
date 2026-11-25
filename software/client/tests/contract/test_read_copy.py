# SPDX-License-Identifier: GPL-3.0-or-later
"""READ and RESULT_READ (T107, read part): the data and their CRC, the chip type from PROBE, a protected chip,
the result kept across a reconnect and dropped by PKG_BEGIN; pdkprog read to Intel HEX and raw files."""
import struct
import zlib

from padauk_prog import cli, ihex
from padauk_prog.protocol import Cmd, Op, ProgrammerError, Stage, Status

from ops_helpers import CHIPS, build, chip_memory, image, program, write_chip_memory


def programmed(pdk_sim, tmp_path, spec="PFS154"):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", spec, "--chip-state", state)
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154")))
    r = program(p)
    assert r.status == Status.OK
    return sim, p, state, r


def test_read_gives_the_chip_content(pdk_sim, tmp_path):
    sim, p, state, prog = programmed(pdk_sim, tmp_path)
    r, words = p.read_chip()   # type from PROBE; read_chip checks data_crc32
    assert r.status == Status.OK and r.chip_id_found == CHIPS["PFS154"]["id"]
    assert r.data_words == 0x800 and words == chip_memory(state, "PFS154")
    assert zlib.crc32(b"".join(w.to_bytes(2, "little") for w in words)) == r.data_crc32
    assert (r.image_crc_raw, r.image_crc_norm) == (prog.image_crc_raw, prog.image_crc_norm)
    r2, words2 = p.read_chip(CHIPS["PFS154"]["id"])   # type given
    assert words2 == words
    p.close()

    p = sim.connect()   # a new session: the result is still there
    assert p.result_read(0, 8) == b"".join(w.to_bytes(2, "little") for w in words[:4])
    try:
        p.result_read(2 * 0x800 - 2, 4)   # past the end
    except ProgrammerError as e:
        assert e.status == Status.BAD_PAYLOAD
    else:
        raise AssertionError("read past the end accepted")
    pkg = build("PFS154", image("PFS154"))
    p.call(Cmd.PKG_BEGIN, pkg[:128])   # the upload takes the slot
    try:
        p.result_read(0, 8)
    except ProgrammerError as e:
        assert e.status == Status.NOT_ALLOWED_NOW
    else:
        raise AssertionError("RESULT_READ after PKG_BEGIN")
    p.close()


def test_protected_chip(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    mem = [0x3FFF] * 0x800
    mem[:4] = [0x1234, 0x0567, 0x2AAA, 0x0001]
    write_chip_memory(state, mem)
    sim = pdk_sim("--chip", "PFS154:protected", "--chip-state", state)
    p = sim.connect()
    r, words = p.read_chip()
    assert r.status == Status.CHIP_PROTECTED and r.stage == Stage.READ and words is None
    status, _ = p.request(Cmd.RESULT_READ, struct.pack("<IH", 0, 8))
    assert status == Status.NOT_ALLOWED_NOW
    p.close()


def test_cli_read(pdk_sim, tmp_path, capsys):
    sim, p, state, prog = programmed(pdk_sim, tmp_path)
    p.close()
    mem = chip_memory(state, "PFS154")
    hexfile, binfile = tmp_path / "out.hex", tmp_path / "out.bin"
    assert cli.main(["-p", sim.url, "read", str(hexfile), "-q"]) == 0
    assert "Read 2048 words from PFS154" in capsys.readouterr().out
    got = ihex.read_words(str(hexfile))
    assert [got[a] for a in range(len(mem))] == mem
    assert cli.main(["-p", sim.url, "read", str(binfile), "-n", "PFS154", "-b", "-q"]) == 0
    assert binfile.read_bytes() == b"".join(w.to_bytes(2, "little") for w in mem)
    p = sim.connect()
    assert p.run_op(Op.PROBE).status == Status.OK
    p.close()


# ---------------------------------------------------------------- COPY (T107, T115, T116)

from padauk_prog.protocol import COPY_SNAP, COPY_TARGET, WARN_CALIB_SNAPPED, Evt  # noqa: E402


def master(pdk_sim, tmp_path, offset=3):
    """A PFS154 master (IHRC, ILRC, BandGap, serial number) calibrated with an oscillator `offset` % off;
    the copies go into the same socket with swap-chip."""
    state = str(tmp_path / "master.bin")
    sim = pdk_sim("--chip", "none", "--chip-state", state)
    sim.proc.stdin.write(f"swap-chip PFS154:offset={offset}\n")
    sim.proc.stdin.flush()
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154", ilrc=True, bg=True)))
    r = program(p)
    assert r.status == Status.OK
    return sim, p, r


def swap(sim, p, spec):
    sim.proc.stdin.write(f"swap-chip {spec}\n")
    sim.proc.stdin.flush()
    p.poll_events(0.1)


def test_copy_modes(pdk_sim, tmp_path):
    sim, p, prog = master(pdk_sim, tmp_path)
    p.poll_events(0)
    r = p.copy(CHIPS["PFS154"]["id"])   # KEEP_VALUE everywhere, BandGap kept
    assert r.status == Status.OK and r.stage == Stage.STORE
    assert any(e.code == Evt.PKG_STATE for e in p.poll_events(0.2))
    assert [c.site_status for c in r.calib] == [4, 4, 4] and r.warnings & 1 and r.warnings & (1 << 5)
    assert [c.master_value for c in r.calib] == [c.value for c in prog.calib]
    info = p.package_info()
    assert info["header"][64:84].rstrip(b"\0").decode() == f"copy PFS154 {r.image_crc_norm:08X}"

    r = p.copy(0, COPY_TARGET, COPY_TARGET, True, 0, 4000000, 31250, 0, "targets")   # type from PROBE
    assert r.status == Status.OK
    assert [(c.kind, c.target_hz, c.site_status) for c in r.calib] == [(1, 4000000, 0), (2, 31250, 0), (3, 0, 0)]
    swap(sim, p, "PFS154:offset=-2")   # a blank chip of the batch
    c = program(p)
    assert c.status == Status.OK
    assert [x.target_hz for x in c.calib] == [4000000, 31250, 0]
    assert abs(c.calib[0].measured_hz - 4000000) <= 40000
    p.close()


def test_snap_standard(pdk_sim, tmp_path):
    sim, p, prog = master(pdk_sim, tmp_path)
    r = p.copy(0, COPY_SNAP)
    assert r.status == Status.OK and r.calib[0].target_hz == 0xFF000000 | prog.calib[0].value
    for offset in (-4, 5):   # copies whose oscillators differ: each lands on the nearest standard 8 MHz
        swap(sim, p, f"PFS154:offset={offset}")
        c = program(p)
        assert c.status == Status.OK and c.warnings & WARN_CALIB_SNAPPED
        assert c.calib[0].target_hz == 8000000 and abs(c.calib[0].measured_hz - 8000000) <= 80000
    p.close()


def test_local_copy_uses_the_configuration(pdk_sim, tmp_path, capsys):
    sim, p, prog = master(pdk_sim, tmp_path)
    p.close()
    assert cli.main(["-p", sim.url, "config", "copy", "--ihrc", "snap", "--bg", "keep"]) == 0
    assert "IHRC snap, ILRC keep, BandGap keep" in capsys.readouterr().out
    p = sim.connect()
    assert p.config_get() == (2, 0, 0)
    before = p.op_result(0).op_id
    sim.proc.stdin.write("local-start copy\n")
    sim.proc.stdin.flush()
    import time
    deadline = time.monotonic() + 20
    while (p.op_result(0) or prog).op_id == before:
        assert time.monotonic() < deadline
        time.sleep(0.05)
    r = p.op_result(0)
    assert r.op_type == Op.COPY and r.source == 1 and r.status == Status.OK
    assert [(c.kind, c.mode) for c in r.calib] == [(1, 2), (2, 0), (3, 0)]
    p.close()


def test_copy_of_a_protected_chip(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    write_chip_memory(state, [0x3FFF] * 0x800)
    sim = pdk_sim("--chip", "PFS154:protected", "--chip-state", state)
    p = sim.connect()
    crc = p.package_info()["crc32"]
    assert p.copy(CHIPS["PFS154"]["id"]).status == Status.CHIP_PROTECTED
    assert p.package_info()["crc32"] == crc
    p.close()
