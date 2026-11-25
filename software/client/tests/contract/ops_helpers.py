# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers for operation contract tests: images with free-pdk placeholders, the simulated chip's memory
(--chip-state file) and what it must hold after PROGRAM, from the independent reference in
software/tests/vectors/gen_footprints.py."""
from __future__ import annotations

import os
import sys

from padauk_prog import package
from padauk_prog.protocol import Op

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests", "vectors")))
import gen_footprints as ref   # noqa: E402  (independent reference: loops, finished forms, placeholders)

CHIPS = {
    "PFS154": dict(id=0xAA1, bits=14, words=0x800, flash=True, fuse_addr=0x7FF, fuse=0x3FFD, res=(0x7E0, 0x7F0)),
    "PMS150C": dict(id=0xA16, bits=13, words=0x400, flash=False, fuse_addr=0x3FF, fuse=0x1FFD, res=(0x3F6, 0x400)),
}
GOTO = {13: 0x1800, 14: 0x3000, 15: 0x6000}
IHRC_SITE, ILRC_SITE, SERIAL_SITE = 0x40, 0x50, 0x60
IHRC_HZ, ILRC_HZ, CAL_MV = 8000000, 62500, 4000


def image(chip: str, ihrc: bool = True, ilrc: bool = False, serial: bool = True, code: int = 0x100) -> dict:
    """{addr: word}: GOTO code with an IHRC (and ILRC) placeholder and a serial number placeholder."""
    c = CHIPS[chip]
    bits = c["bits"]
    w = {a: GOTO[bits] | (a * 37 & 0x3FF) | 1 for a in range(code)}
    regs = ref.CHIPS[chip]["regs"]
    if ihrc:
        for i, x in enumerate(ref.placeholder(bits, ref.IHRC, IHRC_HZ, CAL_MV, regs["ihrcr"])):
            w[IHRC_SITE + i] = x
    else:
        for i in range(10):
            w.pop(IHRC_SITE + i, None)
    if ilrc:
        for i, x in enumerate(ref.placeholder(bits, ref.ILRC, ILRC_HZ, CAL_MV, regs["ilrcr"])):
            w[ILRC_SITE + i] = x
    if serial:
        for i, ch in enumerate("FPSERIAL"):
            w[SERIAL_SITE + i] = ref.RET_IMM[bits] | ord(ch)
    return w


def build(chip: str, words: dict, fuse: bool = True, flags: int = 0) -> bytes:
    c = CHIPS[chip]
    return package.build(c["id"], chip, words, "contract", flags=flags, fuse=c["fuse"] if fuse else None,
                         created_unix=0)


def sites(chip: str, words: dict) -> list:
    """(addr, kind) of the calibration placeholders in an image built by image()."""
    out = []
    if words.get(IHRC_SITE, 0) & 0xFF00 == ref.AND_OP[CHIPS[chip]["bits"]]:
        out.append((IHRC_SITE, ref.IHRC))
    if words.get(ILRC_SITE, 0) & 0xFF00 == ref.AND_OP[CHIPS[chip]["bits"]]:
        out.append((ILRC_SITE, ref.ILRC))
    return out


def expected_memory(chip: str, words: dict, result, fuse: bool = True) -> list:
    """The chip after a successful PROGRAM: k and serial number from the RESULT."""
    c = CHIPS[chip]
    bits, mask = c["bits"], (1 << c["bits"]) - 1
    mem = [mask] * c["words"]
    for a, x in words.items():
        mem[a] = x & mask
    regs = ref.CHIPS[chip]["regs"]
    for i, (addr, kind) in enumerate(sites(chip, words)):
        reg = regs["ihrcr"] if kind == ref.IHRC else regs["ilrcr"]
        loop = ref.loop(bits, kind, addr, [reg, 0, 0])
        mem[addr:addr + 10] = ref.finish(bits, kind, loop, result.calib[i].value)
    if SERIAL_SITE in words and words[SERIAL_SITE] & 0xFF00 == ref.RET_IMM[bits]:
        for i in range(8):
            mem[SERIAL_SITE + i] = ref.RET_IMM[bits] | (result.serial >> (8 * i)) & 0xFF
    if fuse:
        mem[c["fuse_addr"]] = c["fuse"]
    return mem


def chip_memory(path: str, chip: str) -> list:
    """Words of the simulated chip (blank if the state file does not exist yet)."""
    c = CHIPS[chip]
    mask = (1 << c["bits"]) - 1
    if not os.path.exists(path):
        return [mask] * c["words"]
    with open(path, "rb") as f:
        data = f.read()
    return [int.from_bytes(data[2 * i:2 * i + 2], "little") & mask for i in range(c["words"])]


def write_chip_memory(path: str, mem: list) -> None:
    with open(path, "wb") as f:
        f.write(b"".join(x.to_bytes(2, "little") for x in mem))


def counted(chip: str, addr: int) -> bool:
    rs, re_ = CHIPS[chip]["res"]
    return addr == CHIPS[chip]["fuse_addr"] or not rs <= addr <= re_


def program(p, flags: int = 0, on_progress=None):
    return p.run_op(Op.PROGRAM, flags, on_progress=on_progress, timeout=60)
