# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol client: stop-and-wait requests, events, typed commands (contracts/protocol.md)."""
from __future__ import annotations

import struct
import time
import zlib
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
    CAL_START = 0x0040
    CAL_SET = 0x0041
    CAL_CONFIRM = 0x0042
    CAL_GET = 0x0043
    CAL_CLEAR = 0x0044
    SUPPLY_SET = 0x0045
    SUPPLY_GET = 0x0046


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
    CALIB_WRONG_SITE = 0x0415


class Op(IntEnum):
    PROBE = 0x01
    READ = 0x02
    ERASE = 0x03
    BLANK_CHECK = 0x04
    PROGRAM = 0x05
    VERIFY = 0x06
    COPY = 0x07
    RUN = 0x08
    RESTORE_MASTER = 0x09
    PATCH = 0x0A
    SELF_TEST = 0x0B


class Stage(IntEnum):
    PKG_CHECK = 0x01
    ID = 0x02
    ERASE = 0x03
    BLANK_CHECK = 0x04
    WRITE = 0x05
    VERIFY = 0x06
    CALIBRATE = 0x07
    FUSE = 0x08
    READ = 0x09
    STORE = 0x0A
    RUN = 0x0B
    MEASURE = 0x0C
    RESTORE = 0x0D
    SELF_TEST = 0x0E
    FINAL_VERIFY = 0x0F


# OP_START op_flags (PROGRAM)
OPF_SKIP_ERASE = 1 << 0
OPF_SKIP_BLANK_CHECK = 1 << 1
OPF_SKIP_VERIFY = 1 << 2
OPF_CALIB_FINALIZE_ON_FAIL = 1 << 4

# PATCH ext
PATCH_CHECK_ONLY = 1 << 0
PATCH_ALLOW_SECURITY_FUSE = 1 << 1
PATCH_EXPECT_RAW, PATCH_EXPECT_NORMALIZED, PATCH_EXPECT_ACTIVE_PACKAGE = 0, 1, 2
CAP_PATCH = 1 << 6

# COPY ext
COPY_KEEP, COPY_TARGET, COPY_SNAP, COPY_MEASURE = 0, 1, 2, 3
COPY_ALLOW_MODIFY_MASTER = 1 << 0
COPY_ALLOW_MASTER_LOOP = 1 << 1

# RESULT.warnings
WARN_NAMES = ("CALIB_VALUE_COPIED", "CALIB_SNAPPED", "MASTER_MODIFIED", "RESUMED", "CALIB_FORCED",
              "SERIAL_DUPLICATED", "CALIB_UNRECOGNIZED")
WARN_CALIB_VALUE_COPIED = 1 << 0
WARN_CALIB_SNAPPED = 1 << 1
WARN_RESUMED = 1 << 3
WARN_CALIB_FORCED = 1 << 4

SITE_STATUS = {0: "OK", 1: "FAILED", 2: "FORCED", 4: "COPIED", 5: "MASTER_LOOP"}
CALIB_KINDS = {1: "IHRC", 2: "ILRC", 3: "BG"}

EVT_PROGRESS = 0x8001
EVT_OP_DONE = 0x8002
EVT_OP_STARTED = 0x8003


def stage_name(code: int) -> str:
    try:
        return Stage(code).name
    except ValueError:
        return f"0x{code:02X}"


@dataclass
class CalibResult:
    kind: int
    value: int
    target_hz: int
    measured_hz: int
    mode: int
    master_value: int
    site_status: int


@dataclass
class Level:
    rail: int
    set_mV: int
    meas_mV: int
    settle_us: int
    ok: bool


@dataclass
class Result:
    """RESULT of an operation (contracts/protocol.md §4)."""
    op_id: int
    op_type: int
    source: int
    status: int
    stage: int
    chip_id_found: int
    family_found: int
    fail_addr: int
    expected: int
    actual: int
    serial_valid: bool
    serial: int
    fuse_written: int
    data_words: int
    data_crc32: int
    elapsed_ms: int
    calib: list
    image_crc_raw: int
    image_crc_norm: int
    warnings: int
    v_rail: int
    v_set_mV: int
    v_meas_mV: int
    levels: list

    @classmethod
    def parse(cls, p: bytes) -> "Result":
        (op_id, op_type, source, status, stage, chip, fam, fail_addr, expected, actual, serial_valid, serial, fuse,
         data_words, data_crc, elapsed, n) = struct.unpack_from("<HBBHBHBHHHBQHIIIB", p)
        off = 40
        calib = []
        for _ in range(n):
            calib.append(CalibResult(*struct.unpack_from("<BBIIBBB", p, off)))
            off += 13
        raw, norm, warn, v_rail, v_set, v_meas, nl = struct.unpack_from("<IIBBHHB", p, off)
        off += 15
        levels = []
        for _ in range(nl):
            rail, s, m, t, ok = struct.unpack_from("<BHHHB", p, off)
            levels.append(Level(rail, s, m, t, bool(ok)))
            off += 8
        return cls(op_id, op_type, source, status, stage, chip, fam, fail_addr, expected, actual, bool(serial_valid),
                   serial, fuse, data_words, data_crc, elapsed, calib, raw, norm, warn, v_rail, v_set, v_meas, levels)

    @property
    def ok(self) -> bool:
        return self.status == Status.OK

    def warning_names(self) -> list:
        return [n for i, n in enumerate(WARN_NAMES) if self.warnings & (1 << i)]


@dataclass
class Progress:
    op_id: int
    op_type: int
    stage: int
    stage_index: int
    stage_count: int
    percent: int

    @classmethod
    def parse(cls, p: bytes) -> "Progress":
        return cls(*struct.unpack_from("<HBBBBB", p))


@dataclass
class ProgrammerStatus:
    state: int          # 0 idle, 1 upload, 2 operation, 3 running
    op_id: int
    op_type: int
    op_stage: int
    op_percent: int
    pkg_state: int
    pkg_crc32: int
    ok_count: int
    fail_count: int
    flags: int
    copy_cfg: tuple
    selftest_state: int

    @classmethod
    def parse(cls, p: bytes) -> "ProgrammerStatus":
        st, op_id, op_type, stage, pct, pkg, crc, ok, fail, flags, c1, c2, c3, self_test =             struct.unpack_from("<BHBBBBIIIBBBBB", p)
        return cls(st, op_id, op_type, stage, pct, pkg, crc, ok, fail, flags, (c1, c2, c3), self_test)


CAP_MEASURE_POWER = 1 << 4
CAP_SUPPLY_CAL = 1 << 7
CAP_SUPPLY_SET = 1 << 8


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

    # ---------------------------------------------------------------- supply calibration
    def cal_start(self) -> dict:
        """Raises VDD/VPP to the calibration points (no target may be connected)."""
        p = self.call(Cmd.CAL_START, timeout=max(self.timeout, 1.0))
        vdd, vpp, vdd_raw, vpp_raw = struct.unpack_from("<4H", p)
        return {"vdd_set_mV": vdd, "vpp_set_mV": vpp, "vdd_raw_mV": vdd_raw, "vpp_raw_mV": vpp_raw}

    def cal_set(self, vdd_mV: int, vpp_mV: int) -> dict:
        """Sends the voltages read on an external meter; the programmer then applies exact check voltages."""
        p = self.call(Cmd.CAL_SET, struct.pack("<2H", vdd_mV, vpp_mV), timeout=max(self.timeout, 1.0))
        vdd_t, vpp_t, vdd_m, vpp_m = struct.unpack_from("<4H", p)
        return {"vdd_target_mV": vdd_t, "vpp_target_mV": vpp_t, "vdd_meas_mV": vdd_m, "vpp_meas_mV": vpp_m,
                "cal_permille": struct.unpack_from("<4b", p, 8)}

    def cal_confirm(self, accept: bool) -> None:
        self.call(Cmd.CAL_CONFIRM, bytes([1 if accept else 0]))

    def cal_get(self) -> dict:
        p = self.call(Cmd.CAL_GET)
        return {"valid": bool(p[0]), "cal_permille": struct.unpack_from("<4b", p, 1), "state": p[5]}

    def cal_clear(self) -> None:
        self.call(Cmd.CAL_CLEAR)

    # ---------------------------------------------------------------- operations
    def start_op(self, op_type: int, flags: int = 0, chip_id12: int = 0, param: int = 0, ext: bytes = b"") -> int:
        p = self.call(Cmd.OP_START, struct.pack("<BBHH", op_type, flags, chip_id12, param) + ext)
        return struct.unpack_from("<H", p)[0]

    def cancel_op(self, op_id: int) -> None:
        self.call(Cmd.OP_CANCEL, struct.pack("<H", op_id))

    def op_result(self, op_id: int = 0) -> Result | None:
        """RESULT of `op_id` (0 = the last one), None while it is still running."""
        status, data = self.request(Cmd.OP_RESULT, struct.pack("<H", op_id))
        if status == Status.NOT_ALLOWED_NOW:
            return None
        if status != Status.OK:
            raise ProgrammerError(status, data, Cmd.OP_RESULT)
        return Result.parse(data)

    def get_status(self) -> ProgrammerStatus:
        return ProgrammerStatus.parse(self.call(Cmd.GET_STATUS))

    def result_read(self, offset: int, length: int) -> bytes:
        return self.call(Cmd.RESULT_READ, struct.pack("<IH", offset, length))

    def read_chip(self, chip_id12: int = 0, on_progress=None, timeout: float = 120.0) -> tuple:
        """READ, then the data through RESULT_READ: (Result, [words]); raises ProtocolError on a CRC mismatch.
        The words are None if READ failed (see Result.status)."""
        r = self.run_op(Op.READ, chip_id12=chip_id12, on_progress=on_progress, timeout=timeout)
        if r.status != Status.OK:
            return r, None
        total, chunk = 2 * r.data_words, self.max_payload & ~1
        data = bytearray()
        while len(data) < total:
            data += self.result_read(len(data), min(chunk, total - len(data)))
        if zlib.crc32(bytes(data)) != r.data_crc32:
            raise ProtocolError(f"read data CRC 0x{zlib.crc32(bytes(data)):08X} != 0x{r.data_crc32:08X}")
        return r, [int.from_bytes(data[i:i + 2], "little") for i in range(0, total, 2)]

    def copy(self, chip_id12: int = 0, mode_ihrc: int = COPY_KEEP, mode_ilrc: int = COPY_KEEP,
             bg_restore: bool = False, flags: int = 0, target_ihrc_hz: int = 0, target_ilrc_hz: int = 0,
             mV: int = 0, name: str = "", on_progress=None, timeout: float = 300.0) -> Result:
        """COPY the chip in the socket into a new stored package (research R9)."""
        ext = struct.pack("<BBBBIIH", mode_ihrc, mode_ilrc, 1 if bg_restore else 0, flags, target_ihrc_hz,
                          target_ilrc_hz, mV)
        if name:
            ext += name.encode()[:32].ljust(32, bytes(1))
        return self.run_op(Op.COPY, chip_id12=chip_id12, ext=ext, on_progress=on_progress, timeout=timeout)

    def run_start(self, vdd_mV: int = 0) -> int:
        """OP_START(RUN): the chip runs with VDD (0: the package's run_vdd_mV or 5 V) and a UART bridge;
        returns the op_id. Output arrives as EVT_UART_LINK / EVT_UART_RX; OP_CANCEL stops it."""
        return self.start_op(Op.RUN, param=vdd_mV)

    def uart_tx(self, data: bytes, timeout: float = 2.0) -> None:
        """UART_TX in 64-byte pieces (the programmer's TX buffer is small); BUSY (no speed yet, buffer
        full) is retried until `timeout`."""
        for i in range(0, len(data), 64):
            piece = data[i:i + 64]
            deadline = time.monotonic() + timeout
            while True:
                try:
                    self.call(Cmd.UART_TX, piece)
                    break
                except ProgrammerError as e:
                    if e.status != Status.BUSY or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.01)

    def restore_master(self, on_progress=None, timeout: float = 120.0) -> Result:
        """RESTORE_MASTER: writes the stored master dump back after an interrupted MEASURE_MASTER."""
        return self.run_op(Op.RESTORE_MASTER, on_progress=on_progress, timeout=timeout)

    def config_get(self) -> tuple:
        """(mode_ihrc, mode_ilrc, bg_restore) of a local COPY."""
        return tuple(self.call(Cmd.CONFIG_GET)[:3])

    def config_set(self, mode_ihrc: int, mode_ilrc: int, bg_restore: bool) -> None:
        self.call(Cmd.CONFIG_SET, bytes([mode_ihrc, mode_ilrc, 1 if bg_restore else 0]))

    def stats_get(self) -> tuple:
        """(ok, fail): chips programmed since the last reset."""
        return struct.unpack_from("<II", self.call(Cmd.STATS_GET))

    def stats_reset(self) -> None:
        self.call(Cmd.STATS_RESET)

    def wait_op(self, op_id: int, on_progress=None, timeout: float = 600.0, poll_every: float = 1.0) -> Result:
        """Waits for EVT_OP_DONE of `op_id`, passing EVT_PROGRESS to `on_progress`. Asks OP_RESULT when no
        event came for `poll_every` seconds: events are not acknowledged and may be lost."""
        deadline = time.monotonic() + timeout
        quiet_since = time.monotonic()
        others = []   # events that are not about this operation stay for the next poll_events()
        try:
            while time.monotonic() < deadline:
                for e in self.poll_events(0.1):
                    quiet_since = time.monotonic()
                    if e.code == EVT_PROGRESS:
                        pr = Progress.parse(e.payload)
                        if pr.op_id == op_id:
                            if on_progress:
                                on_progress(pr)
                            continue
                    elif e.code == EVT_OP_DONE:
                        r = Result.parse(e.payload)
                        if r.op_id == op_id:
                            return r
                    elif e.code == EVT_OP_STARTED and struct.unpack_from("<H", e.payload)[0] == op_id:
                        continue
                    others.append(e)
                if time.monotonic() - quiet_since >= poll_every:
                    r = self.op_result(op_id)
                    if r is not None:
                        return r
                    quiet_since = time.monotonic()
            raise ProtocolError(f"operation {op_id} did not finish in {timeout:.0f} s")
        finally:
            self.events.extendleft(reversed(others))

    def run_op(self, op_type: int, flags: int = 0, chip_id12: int = 0, param: int = 0, ext: bytes = b"",
               on_progress=None, timeout: float = 600.0) -> Result:
        return self.wait_op(self.start_op(op_type, flags, chip_id12, param, ext), on_progress, timeout)

    def patch(self, chip_id12: int, words: dict, expect_mode: int = PATCH_EXPECT_ACTIVE_PACKAGE,
              expected_crc32: int = 0, check_only: bool = False, allow_security_fuse: bool = False,
              on_progress=None, timeout: float = 120.0) -> Result:
        """PATCH: writes {address: new full value} (only 1→0 changes) after checking the chip content
        against `expected_crc32` (RAW / NORMALIZED) or the active package (ACTIVE_PACKAGE)."""
        flags = (PATCH_CHECK_ONLY if check_only else 0) | (PATCH_ALLOW_SECURITY_FUSE if allow_security_fuse else 0)
        ext = struct.pack("<BBIB", flags, expect_mode, expected_crc32, len(words))
        ext += b"".join(struct.pack("<HH", a, v & 0xFFFF) for a, v in sorted(words.items()))
        return self.run_op(Op.PATCH, chip_id12=chip_id12, ext=ext, on_progress=on_progress, timeout=timeout)

    # ---------------------------------------------------------------- manual supplies
    @staticmethod
    def _supply_state(p: bytes) -> dict:
        vdd, vpp, vdd_m, vpp_m, vdd_lo, vdd_hi, vpp_lo, vpp_hi = struct.unpack_from("<8H", p)
        return {"vdd_set_mV": vdd, "vpp_set_mV": vpp, "vdd_meas_mV": vdd_m, "vpp_meas_mV": vpp_m,
                "vdd_min_mV": vdd_lo, "vdd_max_mV": vdd_hi, "vpp_min_mV": vpp_lo, "vpp_max_mV": vpp_hi}

    def supply_set(self, vdd_mV: int, vpp_mV: int, check: bool = True) -> dict:
        """Applies VDD/VPP (0 = off; nothing may be connected); they drop with the session or after 5 min.
        check=False keeps a rail up even outside the tolerance (diagnostics with a meter)."""
        payload = struct.pack("<2H", vdd_mV, vpp_mV) + (b"" if check else bytes([1]))
        return self._supply_state(self.call(Cmd.SUPPLY_SET, payload, timeout=max(self.timeout, 1.0)))

    def supply_get(self) -> dict:
        """Set points and readings; also keeps manually applied voltages from timing out."""
        return self._supply_state(self.call(Cmd.SUPPLY_GET))

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
