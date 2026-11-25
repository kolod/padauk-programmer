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
