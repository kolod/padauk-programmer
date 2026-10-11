# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-007: power loss in the middle of an upload never leaves a broken package (T046)."""
import random

import pytest

from conftest import iterations
from padauk_prog import package
from padauk_prog.protocol import PkgState, ProgrammerError, ProtocolError
from padauk_prog.transport import TransportError

from pkg_helpers import big_package, vector


def test_power_loss_during_upload(pdk_sim):
    old = vector("ok_pfs154_ihrc")
    new = big_package(seed=3)
    sim = pdk_sim(start=False)
    sim.start()
    p = sim.connect()
    p.upload_package(old)
    p.close()

    rnd = random.Random(2024)
    n = iterations(100, 20)
    outcomes = {"old": 0, "new": 0}
    for i in range(n):
        fail_after = rnd.randrange(0, 12)
        sim.restart("--power-fail-after", str(fail_after))
        try:
            p = sim.connect()
            p.upload_package(new)
            p.close()
        except (ProgrammerError, ProtocolError, TransportError):
            pass
        sim.restart()
        p = sim.connect()
        info = p.package_info()
        assert info["state"] == PkgState.VALID, (i, fail_after)
        crc = info["crc32"]
        assert crc in (package.trailer_crc(old), package.trailer_crc(new)), (i, fail_after)
        outcomes["new" if crc == package.trailer_crc(new) else "old"] += 1
        # put the old package back for the next round
        if crc != package.trailer_crc(old):
            p.upload_package(old)
        p.close()
    assert outcomes["old"] > 0


@pytest.mark.parametrize("fail_after", [0, 1, 2, 3, 4, 5])
def test_simulator_exits_with_99(pdk_sim, fail_after):
    sim = pdk_sim()
    sim.restart("--power-fail-after", str(fail_after))
    p = sim.connect()
    with pytest.raises((ProgrammerError, ProtocolError, TransportError)):
        p.upload_package(big_package())
        p.upload_package(vector("ok_pfs154_3words"))
    assert sim.wait_exit() == 99
