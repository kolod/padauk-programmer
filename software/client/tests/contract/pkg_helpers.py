# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared helpers for package contract tests."""
from __future__ import annotations

import json
import os
import random
import struct
import time

from padauk_prog import package
from padauk_prog.framing import FT_REQ, FT_RSP, Frame, FrameError, to_wire

VECTORS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests", "vectors", "packages"))

JOURNAL_PAGES = (0xF000, 0xF800)   # offsets of the journal pages in the simulator's flash file
RECORD = 40


def manifest() -> list[dict]:
    with open(os.path.join(VECTORS, "manifest.json"), encoding="utf-8") as f:
        return json.load(f)


def vector(name_or_entry) -> bytes:
    e = name_or_entry if isinstance(name_or_entry, dict) else next(m for m in manifest() if m["name"] == name_or_entry)
    with open(os.path.join(VECTORS, e["file"]), "rb") as f:
        return f.read()


def big_package(seed: int = 7) -> bytes:
    """The largest package any supported chip allows: PFS173, every word defined (6660 bytes)."""
    rnd = random.Random(seed)
    words = {a: rnd.randrange(0x5400) & 0x3FFF for a in range(0xBE0)}   # no AND A,# (0x54xx) placeholders
    return package.build(0xEA2, "PFS173", words, "big", created_unix=0)


def journal_records(flash_path: str) -> int:
    with open(flash_path, "rb") as f:
        mem = f.read()
    n = 0
    for page in JOURNAL_PAGES:
        for i in range(2048 // RECORD):
            rec = mem[page + i * RECORD: page + (i + 1) * RECORD]
            if rec != b"\xff" * RECORD:
                n += 1
    return n


def raw_request(prog, seq: int, code: int, payload: bytes = b"", timeout: float = 1.0) -> Frame:
    """Sends a REQ with an explicit seq (bypassing the client's seq counter) and returns its RSP."""
    prog.t.write(to_wire(Frame(FT_REQ, seq, int(code), 0, payload)))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for f in prog._frames(0.05):
            if not isinstance(f, FrameError) and f.type == FT_RSP and f.seq == seq:
                return f
    raise TimeoutError(f"no RSP for seq {seq}")


def u16(b: bytes, off: int = 0) -> int:
    return struct.unpack_from("<H", b, off)[0]


def u32(b: bytes, off: int = 0) -> int:
    return struct.unpack_from("<I", b, off)[0]
