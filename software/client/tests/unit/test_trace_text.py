# SPDX-License-Identifier: GPL-3.0-or-later
"""EVT_TRACE records as text: the calibration readings (kinds 8, 9)."""
from padauk_prog.protocol import TraceRecord


def rec(kind, b8=0, w1=0, w2=0, d=0):
    return TraceRecord(0, kind, b8, w1, w2, d)


def test_calib_point_with_the_reference_count():
    t = rec(8, 0x81, 2000, 2480, 8004500).text()
    assert t == "calib k=0x81: 8004500 Hz (2000 edges, reference 2480, +24.0 %)"


def test_calib_point_without_a_reference():
    assert rec(8, 0x10, 1500, 0, 7000000).text() == "calib k=0x10: 7000000 Hz (1500 edges)"


def test_calib_check():
    assert rec(9, 1, 0x81, 12, 8001000).text().endswith("0.12 % from the search - OK")
    assert "MISMATCH" in rec(9, 2, 0x81, 2400, 6450000).text()


def test_unknown_kind_stays_raw():
    assert rec(42, 1, 2, 3, 4).text() == "42 1 2 3 4"
