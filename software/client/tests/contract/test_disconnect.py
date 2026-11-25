# SPDX-License-Identifier: GPL-3.0-or-later
"""SC-008 (T072): the connection breaks at any moment of PROGRAM; the operation runs to the end, the chip
is programmed as without the break, OP_RESULT(0) after reconnecting gives OK."""
import random
import time

from padauk_prog.protocol import Op, Status

from conftest import iterations
from ops_helpers import build, chip_memory, expected_memory, image, program


def test_breaks_do_not_stop_programming(pdk_sim, tmp_path):
    state = str(tmp_path / "chip.bin")
    sim = pdk_sim("--chip", "PFS154:slow=300", "--chip-state", state)
    p = sim.connect()
    words = image("PFS154")
    p.upload_package(build("PFS154", words))
    ref = program(p)   # undisturbed run: duration and the content every chip must get
    assert ref.status == Status.OK
    p.close()
    rnd = random.Random(8)
    for i in range(iterations(100, 5)):
        p = sim.connect()
        op_id = p.start_op(Op.PROGRAM)
        time.sleep(rnd.uniform(0, ref.elapsed_ms / 1000))
        p.close()   # the host goes away in the middle
        p = sim.connect()
        deadline = time.monotonic() + 30
        r = None
        while r is None:
            assert time.monotonic() < deadline, f"iteration {i}: operation did not finish"
            r = p.op_result(0)
            time.sleep(0.02)
        p.close()
        assert r.op_id == op_id and r.status == Status.OK, (i, r)
        assert r.image_crc_norm == ref.image_crc_norm
        assert chip_memory(state, "PFS154") == expected_memory("PFS154", words, r)
