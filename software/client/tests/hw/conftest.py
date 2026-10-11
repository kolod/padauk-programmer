# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures for tests against a real programmer on a serial port (--hw-port COMx)."""
from __future__ import annotations

import os
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "contract"))   # pkg_helpers

from padauk_prog.protocol import Programmer, connect   # noqa: E402
from padauk_prog.transport import TransportError       # noqa: E402


class Hw:
    def __init__(self, port: str, reset_cmd: str | None):
        self.port, self.reset_cmd = port, reset_cmd
        self.progs: list[Programmer] = []

    def connect(self, **kw) -> Programmer:
        deadline = time.monotonic() + 10
        while True:
            try:
                p = connect(self.port, client="hw-test", **kw)
                self.progs.append(p)
                return p
            except (TransportError, OSError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.3)   # the port comes back after a reset / re-enumeration

    def close_all(self) -> None:
        for p in self.progs:
            try:
                p.close()
            except Exception:
                pass
        self.progs.clear()

    def reset(self) -> None:
        if not self.reset_cmd:
            pytest.skip("needs --hw-reset CMD")
        self.close_all()
        subprocess.run(self.reset_cmd, shell=True, check=True, capture_output=True)
        time.sleep(1.5)

    @property
    def latencies(self):
        return [s for p in self.progs for s in p.latencies]


@pytest.fixture
def hw(request):
    h = Hw(request.config.getoption("--hw-port"), request.config.getoption("--hw-reset"))
    yield h
    h.close_all()
