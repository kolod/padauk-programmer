# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-002 (T073): any single damaged byte of the stored package → PROGRAM stops at PKG_CHECK with
STORE_CORRUPTED and the chip is not touched."""
import os
import random
import shutil

from padauk_prog.protocol import Stage, Status

from conftest import iterations
from ops_helpers import build, image, program


def test_every_damaged_byte_is_caught(pdk_sim, tmp_path):
    clean = str(tmp_path / "clean.bin")
    sim = pdk_sim("--chip", "PFS154", flash=clean)
    p = sim.connect()
    pkg = build("PFS154", image("PFS154", code=0x20))
    p.upload_package(pkg)
    p.close()
    sim.stop()
    with open(clean, "rb") as f:
        mem = f.read()
    base = mem.find(pkg[:16])
    assert base >= 0 and mem[base:base + len(pkg)] == pkg

    offsets = list(range(len(pkg)))
    n = iterations(len(offsets), 12)
    if n < len(offsets):
        offsets = sorted(random.Random(5).sample(offsets, n))
    for off in offsets:
        damaged = str(tmp_path / "damaged.bin")
        shutil.copyfile(clean, damaged)
        with open(damaged, "r+b") as f:
            f.seek(base + off)
            b = f.read(1)[0]
            f.seek(base + off)
            f.write(bytes([b ^ 0x10]))
        state = str(tmp_path / "chip.bin")
        if os.path.exists(state):
            os.remove(state)
        s = pdk_sim("--chip", "PFS154", "--chip-state", state, flash=damaged)
        p = s.connect()
        r = program(p)
        p.close()
        s.stop()
        assert r.status == Status.STORE_CORRUPTED and r.stage == Stage.PKG_CHECK, (off, r)
        assert not os.path.exists(state), off   # nothing was written into the chip
