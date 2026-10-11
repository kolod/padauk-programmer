# SPDX-License-Identifier: GPL-3.0-or-later
"""TCP proxy between the client and pdk-sim that damages COBS frames in both directions.

Faults are applied per frame (a run of bytes ending with 0x00) with independent probabilities:
bit flip, truncation, duplication, drop and delay. The random generator is seeded, so a failing
run can be repeated. `disconnect()` drops the client connection at an arbitrary moment.
"""
from __future__ import annotations

import random
import socket
import threading
import time
from dataclasses import dataclass


@dataclass
class Faults:
    flip: float = 0.0
    truncate: float = 0.0
    duplicate: float = 0.0
    drop: float = 0.0
    delay: float = 0.0
    delay_s: float = 0.02

    @classmethod
    def uniform(cls, p: float) -> "Faults":
        return cls(flip=p, truncate=p, duplicate=p, drop=p)


class _Pipe(threading.Thread):
    def __init__(self, proxy: "FaultProxy", src: socket.socket, dst: socket.socket, faults: Faults, name: str):
        super().__init__(daemon=True, name=name)
        self.proxy, self.src, self.dst, self.faults = proxy, src, dst, faults
        self.buf = bytearray()
        self.rng = random.Random(proxy.seed * 2 + (name == "to_host"))   # str hash is salted per run

    def _send(self, data: bytes) -> None:
        try:
            self.dst.sendall(data)
        except OSError:
            pass

    def _frame(self, frame: bytes) -> None:
        f, r = self.faults, self.rng
        st = self.proxy.stats
        st["frames"] += 1
        if r.random() < f.drop:
            st["dropped"] += 1
            return
        if r.random() < f.flip and len(frame) > 1:
            b = bytearray(frame)
            i = r.randrange(len(b) - 1)          # keep the delimiter in place
            b[i] ^= 1 << r.randrange(8)           # a flip to 0x00 splits the frame — that is fine too
            frame = bytes(b)
            st["flipped"] += 1
        if r.random() < f.truncate and len(frame) > 2:
            frame = frame[:r.randrange(1, len(frame) - 1)] + b"\x00"
            st["truncated"] += 1
        if r.random() < f.delay:
            time.sleep(f.delay_s)
            st["delayed"] += 1
        self._send(frame)
        if r.random() < f.duplicate:
            self._send(frame)
            st["duplicated"] += 1

    def run(self) -> None:
        while not self.proxy._stop.is_set():
            try:
                data = self.src.recv(65536)
            except OSError:
                break
            if not data:
                break
            with self.proxy._lock:
                for b in data:
                    self.buf.append(b)
                    if b == 0:
                        self._frame(bytes(self.buf))
                        self.buf.clear()
        for s in (self.src, self.dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class FaultProxy:
    """Listens on 127.0.0.1:<port>, forwards to `target` (host, port)."""

    def __init__(self, target: tuple[str, int], to_device: Faults | None = None, to_host: Faults | None = None,
                 seed: int = 1):
        self.target = target
        self.to_device = to_device or Faults()
        self.to_host = to_host or Faults()
        self.seed = seed
        self.stats = dict.fromkeys(["frames", "dropped", "flipped", "truncated", "delayed", "duplicated"], 0)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._conns: list[socket.socket] = []
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", 0))
        self.lsock.listen(4)
        self.port = self.lsock.getsockname()[1]
        self._accept = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept.start()

    @property
    def url(self) -> str:
        return f"tcp://127.0.0.1:{self.port}"

    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                c, _ = self.lsock.accept()
            except OSError:
                return
            try:
                d = socket.create_connection(self.target, timeout=5)
            except OSError:
                c.close()
                continue
            for s in (c, d):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.settimeout(None)
            self._conns += [c, d]
            _Pipe(self, c, d, self.to_device, "to_device").start()
            _Pipe(self, d, c, self.to_host, "to_host").start()

    def disconnect(self) -> None:
        """Drops both sides of every open connection (the device sees DTR going low)."""
        for s in self._conns:
            try:
                s.shutdown(socket.SHUT_RDWR)
                s.close()
            except OSError:
                pass
        self._conns.clear()

    def close(self) -> None:
        self._stop.set()
        self.disconnect()
        self.lsock.close()
