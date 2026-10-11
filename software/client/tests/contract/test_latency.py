# SPDX-License-Identifier: GPL-3.0-or-later
"""FR-024: every response within 100 ms, including flash page boundaries, the largest commit and
a journal page switch (T048)."""
from padauk_prog.protocol import Cmd

from pkg_helpers import big_package, vector

LIMIT = 0.100


def test_response_latency(pdk_sim, latency_log):
    sim = pdk_sim()
    p = latency_log.track(sim.connect())
    big = big_package()
    p.upload_package(big)                       # DATA crossing every slot page, largest COMMIT
    small = vector("ok_pfs154_3words")
    for _ in range(2048 // 40 + 2):             # more commits than records in a journal page
        p.upload_package(small)
    p.package_read()
    p.close()

    samples = latency_log.samples
    by_cmd = {}
    for code, dt in samples:
        by_cmd.setdefault(code, []).append(dt)
    for code in (Cmd.PKG_BEGIN, Cmd.PKG_DATA, Cmd.PKG_COMMIT, Cmd.PKG_READ):
        assert by_cmd.get(code), f"no samples for {Cmd(code).name}"
    worst = max(samples, key=lambda s: s[1])
    assert worst[1] <= LIMIT, f"{Cmd(worst[0]).name} took {worst[1] * 1000:.1f} ms"
