# SPDX-License-Identifier: GPL-3.0-or-later
"""Stop-and-wait behaviour of Programmer with a scripted fake transport."""
import pytest

from padauk_prog.framing import FT_EVT, FT_NAK, FT_RSP, Decoder, Frame, to_wire
from padauk_prog.protocol import Cmd, Programmer, ProgrammerError, ProtocolError, Status
from padauk_prog.transport import Transport


class FakeTransport(Transport):
    """`script` is a list of callables, one per request write: f(req_frame) -> bytes to deliver."""

    def __init__(self, script):
        self.script = list(script)
        self.requests = []
        self.pending = b""
        self.dec = Decoder()

    def write(self, data):
        for f in self.dec.feed(data):
            self.requests.append(f)
            action = self.script.pop(0) if self.script else (lambda r: b"")
            self.pending += action(f)

    def read(self, timeout):
        out, self.pending = self.pending, b""
        return out


def ok(payload=b""):
    return lambda r: to_wire(Frame(FT_RSP, r.seq, r.code, 0, payload))


def status(code, payload=b""):
    return lambda r: to_wire(Frame(FT_RSP, r.seq, r.code, code, payload))


def nak(r):
    return to_wire(Frame(FT_NAK, 0xFFFF, 0, Status.FRAME_ERROR))


def corrupt(r):
    w = bytearray(to_wire(Frame(FT_RSP, r.seq, r.code, 0, b"abc")))
    w[3] ^= 0x40
    return bytes(w)


def silent(r):
    return b""


def event(code, payload=b""):
    return to_wire(Frame(FT_EVT, 7, code, 0, payload))


def prog(script, **kw):
    t = FakeTransport(script)
    return Programmer(t, timeout=kw.pop("timeout", 0.05), **kw), t


def test_ok_first_try():
    p, t = prog([ok(b"hi")])
    assert p.call(Cmd.GET_INFO) == b"hi"
    assert len(t.requests) == 1


@pytest.mark.parametrize("failure", [nak, corrupt, silent])
def test_retry_same_seq(failure):
    p, t = prog([failure, ok(b"done")])
    assert p.call(Cmd.GET_INFO) == b"done"
    assert len(t.requests) == 2
    assert t.requests[0] == t.requests[1]   # identical frame, same seq


def test_default_timeout_is_300ms():
    p = Programmer(FakeTransport([]))
    assert p.timeout == pytest.approx(0.3)
    assert p.retries == 5


def test_session_lost_after_5_retries():
    p, t = prog([silent] * 10)
    with pytest.raises(ProtocolError):
        p.call(Cmd.GET_INFO)
    assert len(t.requests) == 6   # first attempt + 5 retries


def test_stale_response_ignored():
    def stale_then_ok(r):
        return to_wire(Frame(FT_RSP, (r.seq - 1) & 0xFFFF, r.code, 0, b"old")) + ok(b"new")(r)

    p, _ = prog([stale_then_ok])
    assert p.call(Cmd.GET_INFO) == b"new"


def test_events_delivered_separately():
    def with_events(r):
        return event(0x8001, b"\x01") + ok(b"rsp")(r) + event(0x8002, b"\x02")

    p, _ = prog([with_events])
    assert p.call(Cmd.GET_INFO) == b"rsp"
    evs = p.poll_events(0.01)
    assert [e.code for e in evs] == [0x8001, 0x8002]
    assert p.poll_events(0) == []


def test_error_status_raises():
    p, _ = prog([status(Status.BUSY, b"\x01")])
    with pytest.raises(ProgrammerError) as e:
        p.call(Cmd.PKG_BEGIN, b"x")
    assert e.value.status == Status.BUSY and e.value.payload == b"\x01"


def test_seq_increments():
    p, t = prog([ok(), ok(), ok()])
    for _ in range(3):
        p.call(Cmd.GET_INFO)
    assert [r.seq for r in t.requests] == [1, 2, 3]
