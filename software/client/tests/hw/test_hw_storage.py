# SPDX-License-Identifier: GPL-3.0-or-later
"""Package storage on real hardware (any platform; no supply front end needed).

    pytest tests/hw --hw-port COM5 [--hw-reset "STM32_Programmer_CLI -c port=SWD -rst"]
"""
import struct
import time

import pytest

from padauk_prog import package
from padauk_prog.protocol import Cmd, PkgState, ProgrammerError, Status

from pkg_helpers import big_package, manifest, raw_request, u16, vector

LATENCY_LIMIT = 0.100   # FR-024


def test_info(hw):
    p = hw.connect()
    i = p.info
    print(f"\n{i}")
    assert (i.proto_major, i.proto_minor) == (1, 0)
    assert i.max_payload == 512
    assert i.max_package_size >= 8192
    assert i.chip_table_size == len(p.list_chips())


def test_every_valid_vector_round_trips(hw):
    p = hw.connect()
    for e in manifest():
        if e["stage"] != "ok":
            continue
        pkg = vector(e)
        res = p.upload_package(pkg)
        assert res["crc32"] == e["crc32"], e["name"]
        assert (res["calib_sites"], res["serial_sites"], res["effective_words"]) == \
            (e["calib_sites"], e["serial_sites"], e["effective_words"]), e["name"]
        assert p.package_read() == pkg, e["name"]
        assert p.package_check()["state"] == PkgState.VALID


def test_every_invalid_vector_gives_exact_code(hw):
    p = hw.connect()
    good = vector("ok_pfs154_ihrc")
    p.upload_package(good)
    for e in manifest():
        if e["stage"] == "ok":
            continue
        with pytest.raises(ProgrammerError) as exc:
            p.upload_package(vector(e))
        assert exc.value.status == e["status"], e["name"]
        if e["stage"] == "commit":
            assert u16(exc.value.payload) == e["addr"], e["name"]
        assert p.package_info()["header"] == good[:128], e["name"]


def test_duplicate_seq_is_not_executed_twice(hw):
    p = hw.connect()
    pkg = vector("ok_pfs154_ihrc")
    s = 1000
    assert raw_request(p, s, Cmd.PKG_BEGIN, pkg[:128]).status == Status.OK
    a = raw_request(p, s + 1, Cmd.PKG_DATA, struct.pack("<I", 0) + pkg[:128])
    b = raw_request(p, s + 1, Cmd.PKG_DATA, struct.pack("<I", 0) + pkg[:128])
    assert a.payload == b.payload == struct.pack("<I", 128)
    assert raw_request(p, s + 2, Cmd.PKG_DATA, struct.pack("<I", 128) + pkg[128:]).status == Status.OK
    c1 = raw_request(p, s + 3, Cmd.PKG_COMMIT)
    c2 = raw_request(p, s + 3, Cmd.PKG_COMMIT)
    assert c1.status == c2.status == Status.OK and c1.payload == c2.payload
    assert raw_request(p, s + 4, Cmd.PKG_COMMIT).status == Status.NOT_ALLOWED_NOW


def test_upload_speed_and_latency(hw):
    """SC-003 (8 KB ≤ 3 s) and FR-024 over back-to-back uploads crossing journal page switches."""
    p = hw.connect()
    big = big_package()
    t0 = time.monotonic()
    p.upload_package(big)
    dt = time.monotonic() - t0
    print(f"\nupload {len(big)} B in {dt * 1000:.0f} ms ({len(big) / dt / 1024:.1f} KB/s)")
    assert dt <= 3.0 * len(big) / 8192

    small = vector("ok_pfs154_3words")
    for i in range(110):   # > 102 records per 4 KB journal page: at least one page switch
        p.upload_package(big if i % 10 == 0 else small)
    by = {}
    for code, d in hw.latencies:
        by.setdefault(Cmd(code).name, []).append(d)
    for name, v in sorted(by.items()):
        v.sort()
        print(f"{name:12} n={len(v):5}  median {v[len(v) // 2] * 1000:6.1f} ms  max {v[-1] * 1000:6.1f} ms")
    worst = max(hw.latencies, key=lambda s: s[1])
    assert worst[1] <= LATENCY_LIMIT, f"{Cmd(worst[0]).name} took {worst[1] * 1000:.1f} ms"


def test_delete(hw):
    p = hw.connect()
    p.upload_package(vector("ok_pfs154_3words"))
    p.package_delete()
    assert p.package_info()["state"] == PkgState.EMPTY
    with pytest.raises(ProgrammerError) as exc:
        p.package_read()
    assert exc.value.status == Status.STORE_EMPTY


def test_package_survives_reset(hw):
    p = hw.connect()
    pkg = vector("ex_helloworld_pfs154")
    p.upload_package(pkg)
    hw.reset()
    p = hw.connect()
    assert p.package_info()["state"] == PkgState.VALID
    assert p.package_read() == pkg
    assert p.package_check()["crc32"] == package.trailer_crc(pkg)

