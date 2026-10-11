# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures for contract tests against pdk-sim: simulator processes, fault proxy, latency log."""
from __future__ import annotations

import os
import socket
import subprocess

import pytest

from padauk_prog.protocol import Programmer, connect

from fault_proxy import FaultProxy, Faults   # tests/contract is on sys.path (rootdir-relative import)


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Sim:
    """One pdk-sim process bound to a flash file; `restart()` keeps the flash (a power cycle)."""

    def __init__(self, exe: str, flash: str, args: list[str]):
        self.exe, self.flash, self.args = exe, flash, list(args)
        self.proc: subprocess.Popen | None = None
        self.port = 0

    def start(self, *extra: str) -> "Sim":
        self.stop()
        self.port = _free_port()
        cmd = [self.exe, "--tcp", f"127.0.0.1:{self.port}", "--flash", self.flash, *self.args, *extra]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True)
        line = self.proc.stdout.readline()
        if not line.startswith("listening"):
            err = self.proc.stderr.read() if self.proc.poll() is not None else ""
            raise RuntimeError(f"pdk-sim did not start: {line!r} {err}")
        return self

    def restart(self, *extra: str) -> "Sim":
        return self.start(*extra)

    def wait_exit(self, timeout: float = 10.0) -> int:
        return self.proc.wait(timeout)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    @property
    def url(self) -> str:
        return f"tcp://127.0.0.1:{self.port}"

    @property
    def address(self) -> tuple[str, int]:
        return ("127.0.0.1", self.port)

    def connect(self, url: str | None = None, **kw) -> Programmer:
        return connect(url or self.url, client="contract-test", **kw)


@pytest.fixture
def pdk_sim(request, tmp_path):
    """Factory: pdk_sim(*args, flash=None) -> started Sim. All processes are killed at teardown."""
    exe = request.config.getoption("--sim")
    sims: list[Sim] = []

    def make(*args: str, flash: str | None = None, start: bool = True) -> Sim:
        s = Sim(exe, flash or str(tmp_path / f"flash{len(sims)}.bin"), list(args))
        sims.append(s)
        return s.start() if start else s

    yield make
    for s in sims:
        s.stop()


@pytest.fixture
def fault_proxy():
    """Factory: fault_proxy(sim, to_device=Faults, to_host=Faults, seed=1) -> FaultProxy."""
    proxies: list[FaultProxy] = []

    def make(sim: Sim, to_device: Faults | None = None, to_host: Faults | None = None, seed: int = 1):
        p = FaultProxy(sim.address, to_device, to_host, seed)
        proxies.append(p)
        return p

    yield make
    for p in proxies:
        p.close()


@pytest.fixture
def latency_log():
    """Collects (command, seconds) from every Programmer passed to `track()`."""
    progs: list[Programmer] = []

    class Log:
        def track(self, prog: Programmer) -> Programmer:
            progs.append(prog)
            return prog

        @property
        def samples(self) -> list[tuple[int, float]]:
            return [s for p in progs for s in p.latencies]

    return Log()


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: long fault-injection runs (reduce with PDK_FAST=1)")


def iterations(full: int, fast: int) -> int:
    return fast if os.environ.get("PDK_FAST") else full
