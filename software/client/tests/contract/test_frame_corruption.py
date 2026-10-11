# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-001: uploads through a lossy link always store an identical package (T045).

1000 uploads of the largest package with >= 1 % damaged/lost/duplicated frames in each direction.
PDK_FAST=1 runs 30.
"""
from conftest import iterations
from fault_proxy import Faults
from padauk_prog import package
from padauk_prog.protocol import PkgState

from pkg_helpers import big_package


def test_uploads_survive_frame_faults(pdk_sim, fault_proxy):
    sim = pdk_sim()
    pkg = big_package()
    n = iterations(1000, 30)
    faults = Faults(flip=0.01, truncate=0.005, duplicate=0.01, drop=0.005, delay=0.005)
    damaged = 0
    for i in range(n):
        proxy = fault_proxy(sim, to_device=faults, to_host=faults, seed=i + 1)
        p = sim.connect(proxy.url, retries=8)
        res = p.upload_package(pkg)
        assert res["crc32"] == package.trailer_crc(pkg), i
        if i % 100 == 0 or i == n - 1:
            assert p.package_read() == pkg, i
        p.close()
        proxy.close()
        damaged += sum(proxy.stats[k] for k in ("dropped", "flipped", "truncated", "duplicated"))
    p = sim.connect()
    info = p.package_info()
    assert info["state"] == PkgState.VALID and info["crc32"] == package.trailer_crc(pkg)
    assert p.package_read() == pkg
    p.close()
    assert damaged > 0
