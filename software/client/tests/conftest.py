# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

VECTORS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "tests", "vectors"))


def pytest_addoption(parser):
    parser.addoption("--sim", action="store", default=os.environ.get("PDK_SIM"),
                     help="path to the pdk-sim executable (enables contract tests)")
    parser.addoption("--hw-port", action="store", default=os.environ.get("PDK_HW_PORT"),
                     help="serial port of a real programmer (enables tests/hw)")
    parser.addoption("--hw-reset", action="store", default=os.environ.get("PDK_HW_RESET"),
                     help="shell command that resets the programmer (e.g. through ST-Link)")


def pytest_collection_modifyitems(config, items):
    skip_sim = pytest.mark.skip(reason="contract tests need --sim PATH")
    skip_hw = pytest.mark.skip(reason="hardware tests need --hw-port PORT")
    for item in items:
        path = str(item.fspath).replace("\\", "/")
        if "/contract/" in path and not config.getoption("--sim"):
            item.add_marker(skip_sim)
        if "/hw/" in path and not config.getoption("--hw-port"):
            item.add_marker(skip_hw)


@pytest.fixture(scope="session")
def vectors_dir():
    return VECTORS
