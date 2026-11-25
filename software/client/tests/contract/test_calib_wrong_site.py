# SPDX-License-Identifier: GPL-3.0-or-later
"""FR-055 (T132, T133): a chip that runs its placeholders out of address order, does not answer the "+1"
steps, or does not drive PA0 for BandGap → CALIB_WRONG_SITE; nothing is written after the measurement
loops; pdkprog explains it."""
import pytest

from padauk_prog import cli
from padauk_prog.protocol import Stage, Status

from ops_helpers import IHRC_SITE, ILRC_SITE, build, chip_memory, image, program, ref


@pytest.mark.parametrize("mode", ["order=reverse", "deaf"])
def test_wrong_site(pdk_sim, tmp_path, capsys, mode):
    state = str(tmp_path / "chip.bin")
    words = image("PFS154", ilrc=True)
    sim = pdk_sim("--chip", f"PFS154:{mode}", "--chip-state", state)
    p = sim.connect()
    p.upload_package(build("PFS154", words))
    r = program(p)
    p.close()
    assert r.status == Status.CALIB_WRONG_SITE and r.stage == Stage.CALIBRATE
    assert [c.site_status for c in r.calib] == [1, 1]
    mem = chip_memory(state, "PFS154")
    assert ref.match_at("PFS154", mem, IHRC_SITE)[0] == ref.LOOP
    assert ref.match_at("PFS154", mem, ILRC_SITE)[0] == ref.LOOP
    assert mem[0x7FF] == 0x3FFF   # no fuse

    assert cli.main(["-p", sim.url, "program", "-q"]) == 1
    assert "address order" in capsys.readouterr().err


def test_right_order_calibrates_both(pdk_sim, tmp_path):
    sim = pdk_sim("--chip", "PFS154:offset=2", "--chip-state", str(tmp_path / "chip.bin"))
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154", ilrc=True)))
    r = program(p)
    p.close()
    assert r.status == Status.OK
    assert [c.site_status for c in r.calib] == [0, 0]
