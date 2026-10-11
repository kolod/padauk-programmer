# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

VECTORS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "tests", "vectors"))


def pytest_addoption(parser):
    parser.addoption("--sim", action="store", default=os.environ.get("PDK_SIM"),
                     help="path to the pdk-sim executable (enables contract tests)")


def pytest_collection_modifyitems(config, items):
    if config.getoption("--sim"):
        return
    skip = pytest.mark.skip(reason="contract tests need --sim PATH")
    for item in items:
        if "contract" in str(item.fspath):
            item.add_marker(skip)


@pytest.fixture(scope="session")
def vectors_dir():
    return VECTORS
