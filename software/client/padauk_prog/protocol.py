# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol client: stop-and-wait requests, events, typed commands (contracts/protocol.md)."""
from __future__ import annotations

import struct
import time
from collections import deque
from dataclasses import dataclass, field
from enum import IntEnum

from .framing import FT_EVT, FT_NAK, FT_REQ, FT_RSP, Decoder, Frame, FrameError, to_wire
from .transport import Transport, TransportError

PROTO_MAJOR = 1
PROTO_MINOR = 0


class Cmd(IntEnum):
    HELLO = 0x0001
    GET_INFO = 0x0002
    GET_STATUS = 0x0003
    LIST_CHIPS = 0x0004
    PKG_BEGIN = 0x0010
    PKG_DATA = 0x0011
    PKG_COMMIT = 0x0012
    PKG_ABORT = 0x0013
    PKG_INFO = 0x0014
    PKG_READ = 0x0015
    PKG_DELETE = 0x0016
    PKG_CHECK = 0x0017
    STATS_GET = 0x0018
    STATS_RESET = 0x0019
    CONFIG_GET = 0x001A
    CONFIG_SET = 0x001B
    OP_START = 0x0020
    OP_CANCEL = 0x0021
    OP_RESULT = 0x0022
    RESULT_READ = 0x0023
    UART_TX = 0x0030


class Evt(IntEnum):
    PROGRESS = 0x8001
    OP_DONE = 0x8002
    OP_STARTED = 0x8003
    UART_RX = 0x8004
    UART_LINK = 0x8005
    PKG_STATE = 0x8006


class Status(IntEnum):
    OK = 0x0000
    FRAME_ERROR = 0x0101
    UNKNOWN_COMMAND = 0x0102
    BAD_PAYLOAD = 0x0103
    PROTO_VERSION = 0x0104
    NO_SESSION = 0x0105
    BUSY = 0x0106
    NOT_ALLOWED_NOW = 0x0107
    INTERNAL = 0x0108
    PKG_CHIP_UNSUPPORTED = 0x0201
    PKG_TOO_LARGE = 0x0202
    PKG_HEADER_INVALID = 0x0203
    PKG_OFFSET = 0x0204
    PKG_CRC_MISMATCH = 0x0205
    PKG_RESERVED_AREA = 0x0206
    PKG_FUSE_CONFLICT = 0x0207
    PKG_SECURITY_NOT_ALLOWED = 0x0208
    PKG_CALIB_UNKNOWN = 0x0209
    PKG_CHIP_READ_ONLY = 0x020B
    PKG_OTP_BLANK_CHECK_REQUIRED = 0x020C
    STORE_EMPTY = 0x0301
    STORE_CORRUPTED = 0x0302
    STORE_WRITE_FAILED = 0x0303
    CHIP_NOT_FOUND = 0x0401
    CHIP_ID_MISMATCH = 0x0402
    CHIP_NOT_BLANK = 0x0403
    VERIFY_FAILED = 0x0404
    CALIB_FAILED = 0x0405
    CHIP_PROTECTED = 0x0406
    VDD_FAULT = 0x0407
    VPP_FAULT = 0x0408
    OP_UNSUPPORTED = 0x0409
    CANCELLED = 0x040A
    HOST_DISCONNECTED = 0x040B
    MASTER_NOT_REWRITABLE = 0x040C
    MASTER_RESTORE_PENDING = 0x040D
    MASTER_MODIFY_NOT_ALLOWED = 0x040E
    PATCH_CONFLICT = 0x040F
    PATCH_IMAGE_MISMATCH = 0x0410
    SELFTEST_CHIP_PRESENT = 0x0411
    SELFTEST_FAILED = 0x0412
    CHIP_ALREADY_PROGRAMMED = 0x0413
    COPY_MASTER_UNCALIBRATED = 0x0414


class PkgState(IntEnum):
    EMPTY = 0
    VALID = 1
    CORRUPTED = 2


FAMILY_NAMES = {0x12: "OTP1_2", 0x21: "OTP2_1", 0x22: "OTP2_2", 0x31: "OTP3_1", 0xF1: "FLASH_1", 0xF2: "FLASH_2"}


def status_name(code: int) -> str:
    try:
        return Status(code).name
    except ValueError:
        return f"0x{code:04X}"


class ProgrammerError(Exception):
    """The programmer answered with a non-OK status."""

    def __init__(self, status: int, payload: bytes = b"", command: int | None = None):
        self.status = status
        self.payload = payload
        self.command = command
        super().__init__(f"{status_name(status)} (0x{status:04X})")


class ProtocolError(Exception):
    """No valid answer after all retries — the session is lost."""


@dataclass
class Info:
    proto_major: int
    proto_minor: int
    max_payload: int
    max_package_size: int
    max_read_words: int
    capabilities: int
    platform: str
    firmware_version: str
    chip_table_size: int

    @classmethod
    def parse(cls, p: bytes) -> "Info":
        ma, mi, mp, mps, mrw, caps = struct.unpack_from("<BBHIHI", p)
        plat = p[14:30].split(b"\0")[0].decode(errors="replace")
        fw = p[30:54].split(b"\0")[0].decode(errors="replace")
        return cls(ma, mi, mp, mps, mrw, caps, plat, fw, p[54])


@dataclass
class ChipDesc:
    index: int
    id12: int
    family: int
    code_words: int
    code_bits: int
    ram_bytes: int
    ops: int
    reserved_start: int
    reserved_end: int
    reserved_first_word: bool
    names: list = field(default_factory=list)

    SIZE = 48

    @classmethod
    def parse(cls, p: bytes) -> "ChipDesc":
        idx, id12, fam, words, bits, ram, ops, rs, re_, rfw = struct.unpack_from("<BHBHBHBHHB", p)
        names = p[15:48].split(b"\0")[0].decode(errors="replace").split("/")
        return cls(idx, id12, fam, words, bits, ram, ops, rs, re_, bool(rfw), names)

    @property
    def name(self) -> str:
        return self.names[0] if self.names else f"0x{self.id12:03X}"

    @property
    def is_flash(self) -> bool:
        return (self.family & 0xF0) == 0xF0

    @property
    def can_read(self) -> bool:
        return bool(self.ops & 1)

    @property
    def can_write(self) -> bool:
        return bool(self.ops & 2)

    @property
    def can_erase(self) -> bool:
        return bool(self.ops & 4)

    def matches(self, name: str) -> bool:
        return any(n.upper() == name.upper() for n in self.names)


class Programmer:
    """Talks to the programmer over a Transport. One outstanding request at a time."""

    def __init__(self, transport: Transport, timeout: float = 0.3, retries: int = 5):
        self.t = transport
        self.timeout = timeout
        self.retries = retries
        self.seq = 0
        self.events: deque[Frame] = deque()
        self.decoder = Decoder()
        self.info: Info | None = None
        self.stats = {"retries": 0, "naks": 0, "bad_frames": 0}
        self.latencies: list[tuple[int, float]] = []   # (command, seconds from the last send to its RSP)

    # ---------------------------------------------------------------- low level
    def _frames(self, timeout: float):
        data = self.t.read(timeout)
        return self.decoder.feed(data) if data else []

    def request(self, code: int, payload: bytes = b"", timeout: float | None = None) -> tuple[int, bytes]:
        """Sends a request and returns (status, payload). Retries on NAK/corruption/timeout."""
        self.seq = (self.seq + 1) & 0xFFFF
        wire = to_wire(Frame(FT_REQ, self.seq, int(code), 0, bytes(payload)))
        tmo = self.timeout if timeout is None else timeout
        for attempt in range(self.retries + 1):
            if attempt:
                self.stats["retries"] += 1
            self.t.write(wire)
            sent_at = time.monotonic()
            deadline = sent_at + tmo
            resend = False
            while not resend:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                rsp = None
                for f in self._frames(min(left, 0.05)):   # whole batch: events after the RSP are kept
                    if isinstance(f, FrameError):
                        self.stats["bad_frames"] += 1
                        resend = rsp is None
                    elif f.type == FT_NAK:
                        self.stats["naks"] += 1
                        resend = rsp is None
                    elif f.type == FT_EVT:
                        self.events.append(f)
                    elif f.type == FT_RSP and f.seq == self.seq and rsp is None:
                        rsp = f
                    # stale RSP of an earlier seq: ignore
                if rsp is not None:
                    self.latencies.append((int(code), time.monotonic() - sent_at))
                    return rsp.status, rsp.payload
        raise ProtocolError(f"no response to 0x{int(code):04X} after {self.retries + 1} attempts")

    def call(self, code: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        status, data = self.request(code, payload, timeout)
        if status != Status.OK:
            raise ProgrammerError(status, data, int(code))
        return data

    def poll_events(self, timeout: float = 0.0) -> list[Frame]:
        """Collects events that arrive within `timeout` (plus any already queued)."""
        deadline = time.monotonic() + timeout
        while True:
            for f in self._frames(max(0.0, min(deadline - time.monotonic(), 0.05))):
                if not isinstance(f, FrameError) and f.type == FT_EVT:
                    self.events.append(f)
            if time.monotonic() >= deadline:
                break
        out = list(self.events)
        self.events.clear()
        return out

    # ---------------------------------------------------------------- session
    def hello(self, client: str = "padauk_prog") -> Info:
        payload = bytes([PROTO_MAJOR, PROTO_MINOR]) + client.encode()[:32]
        status, data = self.request(Cmd.HELLO, payload)
        info = Info.parse(data) if len(data) >= 55 else None
        if status != Status.OK:
            raise ProgrammerError(status, data, Cmd.HELLO)
        self.info = info
        return info

    def get_info(self) -> Info:
        return Info.parse(self.call(Cmd.GET_INFO))

    def list_chips(self) -> list[ChipDesc]:
        chips: list[ChipDesc] = []
        first = 0
        while True:
            p = self.call(Cmd.LIST_CHIPS, bytes([first]))
            total, count = p[0], p[1]
            for i in range(count):
                chips.append(ChipDesc.parse(p[2 + i * ChipDesc.SIZE: 2 + (i + 1) * ChipDesc.SIZE]))
            first += count
            if count == 0 or first >= total:
                return chips

    def find_chip(self, name: str | None = None, id12: int | None = None) -> ChipDesc:
        for c in self.list_chips():
            if (name and c.matches(name)) or (id12 is not None and c.id12 == id12):
                return c
        raise KeyError(name or f"0x{id12:03X}")

    # ---------------------------------------------------------------- package
    @property
    def max_payload(self) -> int:
        return self.info.max_payload if self.info else 512

    def upload_package(self, package: bytes, progress=None) -> dict:
        """PKG_BEGIN / PKG_DATA* / PKG_COMMIT. Returns the commit result; raises ProgrammerError."""
        if len(package) < 128:
            raise ValueError("package too short")
        size = struct.unpack_from("<I", self.call(Cmd.PKG_BEGIN, package[:128]))[0]
        if size != len(package):
            raise ValueError(f"programmer expects {size} bytes, package has {len(package)}")
        chunk = (self.max_payload - 4) & ~1
        off = 0
        while off < len(package):
            part = package[off:off + chunk]
            self.call(Cmd.PKG_DATA, struct.pack("<I", off) + part)
            off += len(part)
            if progress:
                progress(off, len(package))
        p = self.call(Cmd.PKG_COMMIT, timeout=max(self.timeout, 1.0))
        crc, calib, serial, words = struct.unpack_from("<IBBH", p)
        return {"crc32": crc, "calib_sites": calib, "serial_sites": serial, "effective_words": words}

    def package_info(self) -> dict:
        p = self.call(Cmd.PKG_INFO)
        state, crc = struct.unpack_from("<BI", p)
        return {"state": PkgState(state), "crc32": crc, "header": bytes(p[5:133])}

    def package_read(self) -> bytes:
        info = self.package_info()
        if info["state"] == PkgState.EMPTY:
            raise ProgrammerError(Status.STORE_EMPTY)
        size = struct.unpack_from("<I", info["header"], 8)[0]
        out = bytearray()
        while len(out) < size:
            n = min(self.max_payload, size - len(out))
            out += self.call(Cmd.PKG_READ, struct.pack("<IH", len(out), n))
        return bytes(out)

    def package_delete(self) -> None:
        self.call(Cmd.PKG_DELETE)

    def package_check(self) -> dict:
        p = self.call(Cmd.PKG_CHECK)
        state, crc = struct.unpack_from("<BI", p)
        return {"state": PkgState(state), "crc32": crc}

    def close(self) -> None:
        self.t.close()


def connect(spec: str | None, client: str = "pdkprog", **kw) -> Programmer:
    from .transport import open_transport

    prog = Programmer(open_transport(spec), **kw)
    try:
        prog.hello(client)
    except (ProgrammerError, ProtocolError, TransportError):
        prog.close()
        raise
    return prog
