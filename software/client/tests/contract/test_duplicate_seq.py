# SPDX-License-Identifier: GPL-3.0-or-later
"""Retransmitted requests (same seq) are answered from the cache, not executed again (T047)."""
import struct

from padauk_prog.protocol import Cmd, Status

from pkg_helpers import journal_records, raw_request, u32, vector


def test_duplicate_data_and_commit(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    pkg = vector("ok_pfs154_ihrc")
    seq = 100
    r = raw_request(p, seq, Cmd.PKG_BEGIN, pkg[:128])
    assert r.status == Status.OK and u32(r.payload) == len(pkg)

    seq += 1
    first = raw_request(p, seq, Cmd.PKG_DATA, struct.pack("<I", 0) + pkg[:128])
    again = raw_request(p, seq, Cmd.PKG_DATA, struct.pack("<I", 0) + pkg[:128])
    assert first.status == again.status == Status.OK
    assert first.payload == again.payload == struct.pack("<I", 128)

    seq += 1
    r = raw_request(p, seq, Cmd.PKG_DATA, struct.pack("<I", 128) + pkg[128:])
    assert r.status == Status.OK, hex(r.status)   # a duplicated append would have moved the offset

    before = journal_records(sim.flash)
    seq += 1
    c1 = raw_request(p, seq, Cmd.PKG_COMMIT)
    after_first = journal_records(sim.flash)
    c2 = raw_request(p, seq, Cmd.PKG_COMMIT)
    assert c1.status == c2.status == Status.OK and c1.payload == c2.payload
    assert after_first == before + 1
    assert journal_records(sim.flash) == after_first   # no second journal record

    seq += 1
    r = raw_request(p, seq, Cmd.PKG_COMMIT)   # a new seq is a new command
    assert r.status == Status.NOT_ALLOWED_NOW
    p.close()
