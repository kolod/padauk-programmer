# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-009 (T108): COPY with MEASURE_MASTER. The master (oscillator +3 %) is rewritten once per measured
placeholder and once to restore it: the dump before and after is the same, erases +n+1, and copies with
other oscillators (-4 %, 0, +5 %) calibrated to the measured frequency run within 1 % of the master, where
KEEP_VALUE would leave more than 5 %. IHRC + ILRC + BandGap: erases +3, both RC targets measured,
BandGap restored. A placeholder the program never reaches keeps the master's value with a warning."""
from padauk_prog import cli
from padauk_prog.protocol import COPY_ALLOW_MODIFY_MASTER, COPY_KEEP, COPY_MEASURE, Status

from ops_helpers import ILRC_SITE, build, chip_memory, image, program

WARN_VALUE_COPIED, WARN_MASTER_MODIFIED = 1 << 0, 1 << 2


def master(pdk_sim, tmp_path, offset=3):
    state = str(tmp_path / "master.bin")
    sim = pdk_sim("--chip", "none", "--chip-state", state)
    swap(sim, f"PFS154:offset={offset}")
    p = sim.connect()
    p.upload_package(build("PFS154", image("PFS154", ilrc=True, bg=True)))
    r = program(p)
    assert r.status == Status.OK
    return sim, p, state, r


def swap(sim, spec):
    sim.proc.stdin.write(f"swap-chip {spec}\n")
    sim.proc.stdin.flush()


def erases(sim) -> int:
    sim.log.flush()
    with open(sim.flash + ".log") as f:
        return sum(1 for line in f if line.strip() == "chip erase")


def model_hz(nominal, offset, k):
    """SYSCLK of the simulated chip at trim value k (platform/host/chip_model.c)."""
    return nominal * (1 + offset / 100) * (0.75 + k / 512)


def test_measure_master_ihrc(pdk_sim, tmp_path):
    sim, p, state, prog = master(pdk_sim, tmp_path)
    before, e0 = chip_memory(state, "PFS154"), erases(sim)
    r = p.copy(0, COPY_MEASURE, COPY_KEEP, True, COPY_ALLOW_MODIFY_MASTER)
    assert r.status == Status.OK and r.warnings & WARN_MASTER_MODIFIED
    assert erases(sim) - e0 == 2                       # one measurement + the restore
    assert chip_memory(state, "PFS154") == before      # the master is back, bit for bit
    assert p.get_status().flags & 1 == 0
    ihrc = r.calib[0]
    k = prog.calib[0].value
    assert ihrc.mode == COPY_MEASURE and ihrc.site_status == 0 and ihrc.master_value == k
    f = ihrc.measured_hz
    assert f == ihrc.target_hz and abs(f - model_hz(8000000, 3, k)) <= 1

    for offset in (-4, 0, 5):   # copies of the batch: each calibrated to the master's frequency
        swap(sim, f"PFS154:offset={offset}")
        c = program(p)
        assert c.status == Status.OK
        assert c.calib[0].target_hz == f and abs(c.calib[0].measured_hz - f) <= f / 100
    assert abs(model_hz(8000000, -4, k) - f) > f * 5 / 100   # what KEEP_VALUE would give that copy
    p.close()


def test_measure_master_every_site(pdk_sim, tmp_path, capsys):
    sim, p, state, prog = master(pdk_sim, tmp_path)
    before, e0 = chip_memory(state, "PFS154"), erases(sim)
    p.close()
    assert cli.main(["-p", sim.url, "copy", "-q", "--ihrc", "measure", "--ilrc", "measure",
                     "--allow-modify-master", "--yes"]) == 0
    out = capsys.readouterr().out
    assert "measured on the master" in out and "MASTER_MODIFIED" in out
    assert erases(sim) - e0 == 3
    assert chip_memory(state, "PFS154") == before
    p = sim.connect()
    r = p.op_result(0)
    assert [(c.kind, c.mode, c.site_status) for c in r.calib] == [(1, 3, 0), (2, 3, 0), (3, 1, 0)]
    assert abs(r.calib[0].measured_hz - model_hz(8000000, 3, prog.calib[0].value)) <= 1
    assert abs(r.calib[1].measured_hz - model_hz(62500, 3, prog.calib[1].value)) <= 1
    swap(sim, "PFS154:offset=-2")
    c = program(p)
    assert c.status == Status.OK
    for i in (0, 1):
        assert abs(c.calib[i].measured_hz - r.calib[i].measured_hz) <= r.calib[i].measured_hz / 100
    p.close()


def test_site_the_program_never_reaches(pdk_sim, tmp_path):
    sim, p, state, prog = master(pdk_sim, tmp_path)
    p.close()
    sim.restart("--chip", f"PFS154:offset=3,halt=0x{ILRC_SITE:X}")   # the same master; it stops before ILRC
    before, e0 = chip_memory(state, "PFS154"), erases(sim)
    p = sim.connect()
    r = p.copy(0, COPY_MEASURE, COPY_MEASURE, True, COPY_ALLOW_MODIFY_MASTER)
    assert r.status == Status.OK
    assert r.warnings & WARN_VALUE_COPIED and r.warnings & WARN_MASTER_MODIFIED
    assert [(c.kind, c.site_status) for c in r.calib] == [(1, 0), (2, 4), (3, 0)]
    assert r.calib[1].master_value == prog.calib[1].value
    assert erases(sim) - e0 == 3 and chip_memory(state, "PFS154") == before
    p.close()


def test_measure_needs_permission_and_a_flash_master(pdk_sim, tmp_path):
    sim, p, state, prog = master(pdk_sim, tmp_path)
    assert p.copy(0, COPY_MEASURE).status == Status.MASTER_MODIFY_NOT_ALLOWED
    p.close()
    assert cli.main(["-p", sim.url, "copy", "--ihrc", "measure"]) != 0   # pdkprog asks for the flag
