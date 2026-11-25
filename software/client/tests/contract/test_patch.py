# SPDX-License-Identifier: GPL-3.0-or-later
"""PATCH on programmed OTP chips (T078): a compatible patch against the stored package changes and verifies
only the given words; a batch with different calibration and serial numbers shares one normalized checksum;
another firmware → PATCH_IMAGE_MISMATCH, a 0→1 change → PATCH_CONFLICT, both without writing; --check shows
the sums and writes nothing."""
import re

from padauk_prog import cli
from padauk_prog.protocol import PATCH_EXPECT_NORMALIZED, Status

from ops_helpers import CHIPS, build, chip_memory, image, program

CHIP = "PMS150C"


def programmed_chip(pdk_sim, tmp_path, name, offset=0, words=None):
    """A PMS150C programmed with the package; returns (sim, state file, RESULT)."""
    state = str(tmp_path / f"{name}.bin")
    flash = str(tmp_path / "flash.bin")
    sim = pdk_sim("--chip", f"{CHIP}:offset={offset}", "--chip-state", state, flash=flash)
    p = sim.connect()
    p.upload_package(build(CHIP, words or image(CHIP)))
    r = program(p)
    p.close()
    assert r.status == Status.OK
    return sim, state, r


def sums(out):
    m = re.search(r"CRC 0x([0-9A-F]{8}), normalized 0x([0-9A-F]{8})", out)
    return int(m.group(1), 16), int(m.group(2), 16)


def test_compatible_patch_against_the_package(pdk_sim, tmp_path, capsys):
    sim, state, r = programmed_chip(pdk_sim, tmp_path, "a")
    before = chip_memory(state, CHIP)
    new = before[0x20] & ~0x0003
    assert cli.main(["-p", sim.url, "patch", "-n", CHIP, f"0x20={new:#x}", "--expect-package", "-q"]) == 0
    assert "Patched 1 word(s), verified" in capsys.readouterr().out
    after = chip_memory(state, CHIP)
    assert after[0x20] == new
    assert [a for a in range(len(after)) if after[a] != before[a]] == [0x20]


def test_batch_shares_the_normalized_checksum(pdk_sim, tmp_path, capsys):
    norms = set()
    for i, offset in enumerate((-3, 0, 4)):
        sim, state, r = programmed_chip(pdk_sim, tmp_path, f"chip{i}", offset)
        assert cli.main(["-p", sim.url, "patch", "-n", CHIP, "--check", "-q"]) == 0
        raw, norm = sums(capsys.readouterr().out)
        assert (raw, norm) == (r.image_crc_raw, r.image_crc_norm)
        norms.add(norm)
        p = sim.connect()
        word = chip_memory(state, CHIP)[0x21] & ~1
        res = p.patch(CHIPS[CHIP]["id"], {0x21: word}, PATCH_EXPECT_NORMALIZED, norm)
        p.close()
        assert res.status == Status.OK and res.data_words == 1
        sim.stop()
    assert len(norms) == 1


def test_other_firmware_and_conflict_write_nothing(pdk_sim, tmp_path, capsys):
    other = image(CHIP)
    other[0x05] ^= 0x0002   # another program for the same chip
    sim, state, r = programmed_chip(pdk_sim, tmp_path, "other", words=other)
    p = sim.connect()
    p.upload_package(build(CHIP, image(CHIP)))   # the stored package is now the real one
    p.close()
    before = chip_memory(state, CHIP)
    assert cli.main(["-p", sim.url, "patch", "-n", CHIP, f"0x20={before[0x20] & ~1:#x}", "--expect-package",
                     "-q"]) == 1
    assert "PATCH_IMAGE_MISMATCH" in capsys.readouterr().err
    assert chip_memory(state, CHIP) == before

    zero = next(b for b in range(13) if not before[0x20] >> b & 1)   # a 0 the patch wants back as 1
    assert cli.main(["-p", sim.url, "patch", "-n", CHIP, f"0x20={before[0x20] | 1 << zero:#x}",
                     "--expect-crc", f"{r.image_crc_raw:#x}", "-q"]) == 1
    err = capsys.readouterr().err
    assert "PATCH_CONFLICT" in err and "0x0020" in err and f"{before[0x20]:013b}" in err
    assert chip_memory(state, CHIP) == before


def test_check_writes_nothing(pdk_sim, tmp_path, capsys):
    sim, state, r = programmed_chip(pdk_sim, tmp_path, "c")
    before = chip_memory(state, CHIP)
    assert cli.main(["-p", sim.url, "patch", "-n", CHIP, f"0x22={before[0x22] & ~1:#x}", "--expect-package",
                     "--check", "-q"]) == 0
    out = capsys.readouterr().out
    assert "Check passed, nothing written" in out
    assert sums(out) == (r.image_crc_raw, r.image_crc_norm)
    assert chip_memory(state, CHIP) == before
    assert cli.main(["-p", sim.url, "patch", "-n", CHIP, "0x22=0"]) == 1   # no expected sum and no --check
    assert "--expect-crc" in capsys.readouterr().err
