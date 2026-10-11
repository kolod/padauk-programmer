# SPDX-License-Identifier: GPL-3.0-or-later
"""COBS framing and frame encoding (contracts/protocol.md §1)."""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

FT_REQ = 0x01
FT_RSP = 0x02
FT_EVT = 0x03
FT_NAK = 0x04

HEADER = struct.Struct("<BBHHHH")
OVERHEAD = HEADER.size + 4
PROTO_MAX_PAYLOAD = 1024


class FrameError(Exception):
    """A frame on the wire was malformed (COBS, length or CRC)."""


@dataclass(frozen=True)
class Frame:
    type: int
    seq: int
    code: int
    status: int = 0
    payload: bytes = b""
    flags: int = 0


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    """Encodes `data` and appends the 0x00 delimiter."""
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    out.append(0)
    return bytes(out)


def cobs_decode(block: bytes) -> bytes:
    """Decodes one COBS block (without delimiter)."""
    out = bytearray()
    i = 0
    n = len(block)
    while i < n:
        code = block[i]
        i += 1
        if code == 0 or i + code - 1 > n:
            raise FrameError("bad COBS block")
        chunk = block[i:i + code - 1]
        if 0 in chunk:
            raise FrameError("zero inside COBS block")
        out += chunk
        i += code - 1
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


def frame_encode(f: Frame) -> bytes:
    head = HEADER.pack(f.type, f.flags, f.seq & 0xFFFF, f.code & 0xFFFF, f.status & 0xFFFF, len(f.payload))
    body = head + bytes(f.payload)
    return body + struct.pack("<I", crc32(body))


def frame_decode(raw: bytes, max_payload: int = PROTO_MAX_PAYLOAD) -> Frame:
    if len(raw) < OVERHEAD:
        raise FrameError("frame too short")
    ftype, flags, seq, code, status, plen = HEADER.unpack_from(raw)
    if plen > max_payload or len(raw) != OVERHEAD + plen:
        raise FrameError("length mismatch")
    body = raw[:HEADER.size + plen]
    (crc,) = struct.unpack_from("<I", raw, HEADER.size + plen)
    if crc != crc32(body):
        raise FrameError("CRC mismatch")
    return Frame(ftype, seq, code, status, bytes(raw[HEADER.size:HEADER.size + plen]), flags)


def to_wire(f: Frame) -> bytes:
    return cobs_encode(frame_encode(f))


class Decoder:
    """Streaming receiver: feed bytes, get frames (or FrameError objects for broken ones)."""

    def __init__(self, max_encoded: int = 4096):
        self._buf = bytearray()
        self._max = max_encoded
        self._overflow = False

    def feed(self, data: bytes) -> list:
        out = []
        for b in data:
            if b != 0:
                if len(self._buf) < self._max:
                    self._buf.append(b)
                else:
                    self._overflow = True
                continue
            block, overflow = bytes(self._buf), self._overflow
            self._buf.clear()
            self._overflow = False
            if overflow:
                out.append(FrameError("frame too long"))
                continue
            if not block:
                continue
            try:
                out.append(frame_decode(cobs_decode(block)))
            except FrameError as e:
                out.append(e)
        return out
