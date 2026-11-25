# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-012 (T074): power lost at any point of PROGRAM on an OTP chip; PROGRAM again — on the same programmer
or on another one with the same package — completes it (OK, RESUMED) or refuses without touching it
(CHIP_NOT_BLANK). No bit is ever cleared that the image needs set; a written serial number is kept."""
import random
import shutil
import subprocess

from padauk_prog.protocol import ProgrammerError, ProtocolError, Status, WARN_RESUMED
from padauk_prog.transport import TransportError

from conftest import iterations
from ops_helpers import (CHIPS, SERIAL_SITE, build, chip_memory, counted, expected_memory, image, program,
                         ref, sites)

CHIP = "PMS150C"
BITS = CHIPS[CHIP]["bits"]
MASK = (1 << BITS) - 1


def free_words(words):
    """Addresses whose final value is known before programming (not a calibration site, not the serial)."""
    skip = set()
    for addr, kind in sites(CHIP, words):
        skip.update(range(addr, addr + 10))
    skip.update(range(SERIAL_SITE, SERIAL_SITE + 8))
    return skip


def test_power_loss_and_retry(pdk_sim, tmp_path):
    words = image(CHIP)
    clean = str(tmp_path / "flash.bin")
    s = pdk_sim("--chip", CHIP, flash=clean)
    p = s.connect()
    p.upload_package(build(CHIP, words))
    p.close()
    s.stop()

    target = [MASK] * CHIPS[CHIP]["words"]   # what the image needs, outside the sites
    for a, x in words.items():
        target[a] = x & MASK
    target[CHIPS[CHIP]["fuse_addr"]] = CHIPS[CHIP]["fuse"]
    skip = free_words(words)

    rnd = random.Random(12)
    counts = {"cut": 0, "resumed": 0, "refused": 0}
    for i in range(iterations(100, 6)):
        state = str(tmp_path / f"chip{i}.bin")
        first = str(tmp_path / f"flash{i}.bin")
        shutil.copyfile(clean, first)
        s = pdk_sim("--chip", CHIP, "--chip-state", state, "--power-fail-after", str(rnd.randrange(0, 160)),
                    flash=first)
        try:
            p = s.connect()
            program(p)
            p.close()
        except (ProtocolError, TransportError, ProgrammerError, OSError):
            pass
        try:
            cut = s.proc.wait(timeout=0.5) == 99
        except subprocess.TimeoutExpired:
            cut = False   # the count ran past the end: no power loss
        s.stop()
        counts["cut"] += cut
        before = chip_memory(state, CHIP)
        for a in range(len(before)):   # never a 0 where the image needs a 1
            if counted(CHIP, a) and a not in skip:
                assert target[a] & ~before[a] == 0, (i, hex(a))

        # retry: on the same programmer, or on another one holding the same package
        second = first if i % 2 == 0 else str(tmp_path / f"other{i}.bin")
        if second != first:
            shutil.copyfile(clean, second)
        s = pdk_sim("--chip", CHIP, "--chip-state", state, flash=second)
        p = s.connect()
        r = program(p)
        p.close()
        s.stop()
        after = chip_memory(state, CHIP)
        if r.status == Status.CHIP_NOT_BLANK:
            counts["refused"] += 1
            assert after == before, i
            continue
        assert r.status in (Status.OK, Status.CHIP_ALREADY_PROGRAMMED), (i, r)
        if r.status == Status.CHIP_ALREADY_PROGRAMMED:
            continue
        assert after == expected_memory(CHIP, words, r), i
        if any(before[a] != MASK for a in range(len(before)) if counted(CHIP, a)):
            assert r.warnings & WARN_RESUMED, i
            counts["resumed"] += 1
        serial_before = before[SERIAL_SITE:SERIAL_SITE + 8]
        if all(w & 0xFF00 == ref.RET_IMM[BITS] for w in serial_before):
            assert r.serial == sum((w & 0xFF) << (8 * k) for k, w in enumerate(serial_before)), i
    assert counts["cut"] >= 1 and counts["resumed"] + counts["refused"] >= 1, counts
