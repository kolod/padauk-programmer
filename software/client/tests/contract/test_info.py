# SPDX-License-Identifier: GPL-3.0-or-later
"""INFO, LIST_CHIPS and versions (T094): limits and capabilities, paged chip list, a major version the
programmer does not speak, no operations before HELLO; SC-006 — a chip added only to the firmware
(pdk-sim-extra) is listed and programmed by this unchanged client."""
import os
import struct

import pytest

from padauk_prog import cli, ihex
from padauk_prog.protocol import (CAP_MEASURE_MASTER, CAP_MEASURE_POWER, CAP_PATCH, CAP_SUPPLY_CAL, CAP_SUPPLY_SET,
                                  ChipDesc, Cmd, Op, Programmer, Status)
from padauk_prog.transport import open_transport

from ops_helpers import image


def test_info_and_chip_list(pdk_sim):
    sim = pdk_sim()
    p = sim.connect()
    i = p.info
    assert (i.proto_major, i.proto_minor) == (1, 1)
    assert i.max_payload == 512 and i.max_package_size == 8192 and i.max_read_words == 4096
    for cap in (CAP_MEASURE_POWER, CAP_SUPPLY_CAL, CAP_SUPPLY_SET, CAP_PATCH, CAP_MEASURE_MASTER, 1 << 0, 1 << 1,
                1 << 2, 1 << 3, 1 << 5):
        assert i.capabilities & cap
    chips = p.list_chips()
    assert len(chips) == i.chip_table_size
    assert [c.index for c in chips] == list(range(len(chips)))
    page = p.call(Cmd.LIST_CHIPS, bytes([3]))   # paging from an index
    assert page[0] == len(chips) and ChipDesc.parse(page[2:2 + ChipDesc.SIZE]).index == 3
    assert p.call(Cmd.LIST_CHIPS, bytes([len(chips)]))[1] == 0   # past the end: empty page
    p.close()


def test_versions_and_session(pdk_sim):
    sim = pdk_sim()
    p = Programmer(open_transport(sim.url))   # no HELLO yet
    for code, payload in ((Cmd.OP_START, struct.pack("<BBHH", Op.PROBE, 0, 0, 0)), (Cmd.PKG_BEGIN, bytes(128))):
        status, _ = p.request(code, payload)
        assert status == Status.NO_SESSION
    status, data = p.request(Cmd.GET_INFO)   # allowed without a session
    assert status == Status.OK and data[0] == 1
    status, _ = p.request(Cmd.HELLO, bytes([2, 0]) + b"future-client")
    assert status == Status.PROTO_VERSION
    status, _ = p.request(Cmd.OP_START, struct.pack("<BBHH", Op.PROBE, 0, 0, 0))
    assert status == Status.NO_SESSION   # the refused HELLO did not open a session
    p.close()


def test_new_chip_needs_only_firmware(request, pdk_sim, tmp_path, capsys):
    exe = request.config.getoption("--sim")
    root, ext = os.path.splitext(exe)
    extra = root + "-extra" + ext
    if not os.path.exists(extra):
        pytest.skip("pdk-sim-extra is not built")
    sim = pdk_sim("--chip", "PTEST1:offset=2", "--chip-state", str(tmp_path / "chip.bin"), exe=extra)
    assert cli.main(["-p", sim.url, "list"]) == 0
    assert "PTEST1" in capsys.readouterr().out
    words = image("PFS154")   # PTEST1 is a 14-bit FLASH chip with the PFS154 registers
    hexfile = tmp_path / "fw.hex"
    hexfile.write_text(ihex.format_words({a: w for a, w in words.items() if a < 0x3E0}))
    assert cli.main(["-p", sim.url, "write", str(hexfile), "-n", "PTEST1", "-q"]) == 0
    assert "Programming done" in capsys.readouterr().out
