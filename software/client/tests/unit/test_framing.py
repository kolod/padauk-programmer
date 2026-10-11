# SPDX-License-Identifier: GPL-3.0-or-later
"""Client framing against the shared vectors (tests/vectors/frames.json)."""
import json
import os

import pytest

from padauk_prog.framing import (Decoder, Frame, FrameError, cobs_decode, cobs_encode, crc32, frame_decode,
                                 frame_encode, to_wire)


@pytest.fixture(scope="module")
def vec(vectors_dir):
    with open(os.path.join(vectors_dir, "frames.json")) as f:
        return json.load(f)


def test_crc32(vec):
    for v in vec["crc32"]:
        assert crc32(bytes.fromhex(v["data"])) == v["crc"]


def test_cobs(vec):
    for v in vec["cobs"]:
        raw, enc = bytes.fromhex(v["raw"]), bytes.fromhex(v["encoded"])
        assert cobs_encode(raw) == enc
        assert cobs_decode(enc[:-1]) == raw


def test_frames(vec):
    for v in vec["frames"]:
        f = Frame(v["type"], v["seq"], v["code"], v["status"], bytes.fromhex(v["payload"]))
        assert frame_encode(f).hex() == v["decoded"], v["name"]
        assert to_wire(f).hex() == v["encoded"], v["name"]
        assert frame_decode(bytes.fromhex(v["decoded"])) == f
        assert Decoder().feed(bytes.fromhex(v["encoded"])) == [f]


def test_decoder_split_and_resync(vec):
    wires = [bytes.fromhex(v["encoded"]) for v in vec["frames"]]
    stream = b"\x13\x37garbage\x00" + b"".join(wires)
    d = Decoder()
    out = []
    for b in stream:   # byte by byte
        out += d.feed(bytes([b]))
    assert isinstance(out[0], FrameError)
    assert out[1:] == [frame_decode(bytes.fromhex(v["decoded"])) for v in vec["frames"]]


def test_corruption_detected(vec):
    v = vec["frames"][0]
    raw = bytearray(bytes.fromhex(v["decoded"]))
    for i in range(len(raw)):
        for bit in range(8):
            bad = bytearray(raw)
            bad[i] ^= 1 << bit
            with pytest.raises(FrameError):
                frame_decode(bytes(bad))


def test_length_checks():
    f = frame_encode(Frame(2, 1, 1, 0, b"x" * 10))
    with pytest.raises(FrameError):
        frame_decode(f[:-1])
    with pytest.raises(FrameError):
        frame_decode(f, max_payload=9)
    with pytest.raises(FrameError):
        frame_decode(b"\x01\x02")
