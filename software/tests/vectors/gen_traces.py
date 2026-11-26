#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference traces for the chip engine (specs/002-chip-engine/contracts/traces.md, constitution VI).

Written from docs/easy-pdk-programmer-protocol.md (§4 sequences, §9 IC table) independently of the engine's C code: the
chip parameters below are transcribed from the document, not taken from the firmware's chip table. Each trace is the
expected sequence of HAL events (hal_icp.h calls and supply set points) for one engine primitive; the unit test
software/firmware/tests/test_engine_trace.c replays the primitive on the fake HAL and compares.

Usage: python gen_traces.py            # write traces/*.json
       python gen_traces.py --check    # fail if traces/ is out of date
"""
import argparse
import json
import pathlib
import sys

OUT = pathlib.Path(__file__).resolve().parent / "traces"

# §9: name -> (id12, family, addr_bits, data_bits, words, read cmd/hv (vdd, vpp, vdd, vpp) mV,
#              write cmd/hv, block (size, groups, clocks), erase cmd/hv, erase clocks)
CHIPS = {
    "PFS154": dict(id=0xAA1, fam="FLASH_1", ab=13, db=14, words=0x800, rd=(2500, 5500, 2500, 5500),
                   wr=(2500, 5500, 5800, 8500), blk=(4, 1, 8), er=(2500, 5500, 3000, 9000), erc=2),
    "PFS172": dict(id=0xCA6, fam="FLASH_2", ab=13, db=14, words=0x800, rd=(2500, 5500, 2500, 0),
                   wr=(2500, 5500, 5300, 0), blk=(4, 1, 4), er=(2500, 5500, 5300, 0), erc=14),
    "PFS173": dict(id=0xEA2, fam="FLASH_1", ab=13, db=15, words=0xC00, rd=(2500, 5500, 2500, 5500),
                   wr=(2500, 6500, 5800, 9000), blk=(4, 1, 8), er=(2500, 5500, 3000, 9000), erc=4),
    "PMS150C": dict(id=0xA16, fam="OTP1_2", ab=12, db=13, words=0x400, rd=(3000, 5000, 3000, 5000),
                    wr=(4300, 8000, 5800, 10500), blk=(2, 1, 8)),
    "PMS152": dict(id=0xA27, fam="OTP2_2", ab=12, db=14, words=0x500, rd=(3000, 5000, 3000, 5000),
                   wr=(4300, 8000, 5800, 10500), blk=(2, 1, 8)),
    "PMS133": dict(id=0xC19, fam="OTP2_1", ab=12, db=15, words=0x1000, rd=(3000, 5000, 3000, 5000)),
    "PMC251": dict(id=0x058, fam="OTP3_1", ab=13, db=16, words=0x400, rd=(2500, 5000, 2500, 5500)),
}

FLASH = ("FLASH_1", "FLASH_2")
# §4.4 command codes
CMD = {"FLASH_1": {"read": 0x6, "write": 0x7, "erase": 0x3}, "FLASH_2": {"read": 0xC, "write": 0x7, "erase": 0x5}}
for fam in ("OTP1_2", "OTP2_1", "OTP2_2", "OTP3_1"):
    CMD[fam] = {"read": 0x6, "write": 0x7}

PROBE_VPP, PROBE_VDD = 4500, 2000     # §4.6
PAUSE_LEAVE = 10_000                  # §4.3: 10 ms after leaving (+100 ms after erase/write)
PAUSE_LEAVE_HV = 110_000


class Trace:
    def __init__(self):
        self.ev = []
        self.v = {"vdd": 0, "vpp": 0}

    def add(self, e, **kw):
        if e in self.v:
            self.v[e] = kw["mV"]
        self.ev.append(dict(e=e, **kw))

    # §4.3 enter: lines idle, PA3 output 0, VPP (≥100 µs), VDD (≥500 µs), family lines
    def enter(self, fam, vpp, vdd, gap=0):
        self.add("idle")
        self.add("mode", line="CLK", dir="out_low")
        if gap:
            self.add("vpp", mV=vpp, min_us=100, gap_min_us=gap)
        else:
            self.add("vpp", mV=vpp, min_us=100)
        self.add("vdd", mV=vdd, min_us=500)
        if fam in FLASH:
            self.add("mode", line="DAT", dir="out_low")
        else:
            self.add("mode", line="DAT", dir="in_pulldown")
            self.add("mode", line="PA4", dir="out_low")
            if fam == "OTP3_1":
                self.add("mode", line="PA0", dir="out_low")
                self.add("mode", line="PA7", dir="out_low")

    # §4.3 leave: lines to inputs, VDD = 0, VPP = 0 without a delay
    def leave(self):
        self.add("idle")
        for rail in ("vdd", "vpp"):   # a supply already at 0 (PFS172 VPP) does not change
            if self.v[rail]:
                self.add(rail, mV=0)

    # §4.4 the command preamble
    def command(self, fam, op):
        code = CMD[fam].get(op, 0)
        if fam in FLASH:
            self.add("send", data="DAT", clk="CLK", value=0xA5A5A5A0 | code, bits=32)
        elif fam == "OTP1_2":
            self.add("send", data="PA4", clk="CLK", value=0xA5A5A5A0 | code, bits=32)
            self.add("mode", line="PA4", dir="out_low")
        elif fam in ("OTP2_1", "OTP2_2"):
            self.add("send", data="PA4", clk="CLK", value=0x5A5A5A50 | code, bits=32)
            self.add("mode", line="PA4", dir="out_low")
        else:  # OTP3_1: no command code
            self.add("send", data="PA4", clk="CLK", value=0x5A5A5A5A, bits=32)
            self.add("send", data="PA4", clk="CLK", value=0, bits=27)
            self.add("mode", line="PA4", dir="out_low")

    # §4.4 FLASH ACK: PA6 input, 16 (FLASH_2: 17) bits, PA6 output, +1 clock
    def ack(self, bits, reply):
        self.add("mode", line="DAT", dir="in")
        self.add("recv", data="DAT", clk="CLK", bits=bits, reply=reply)
        self.add("mode", line="DAT", dir="out_low")
        self.add("clock", min_ns=1000)

    # §4.5 OTP GetID bits
    def otp_id(self, fam, data_bits, reply):
        n = data_bits + 12 if fam == "OTP2_1" else data_bits + 13 if fam == "OTP3_1" else 2 * data_bits + 12
        self.add("recv", data="DAT", clk="PA0" if fam == "OTP3_1" else "CLK", bits=n, reply=reply)

    # §4.7 one word
    def read_word(self, c, addr, word):
        fam = c["fam"]
        if fam in FLASH:
            self.add("send", data="DAT", clk="CLK", value=addr, bits=c["ab"])
            self.add("mode", line="DAT", dir="in")
            self.add("mode", line="CLK", dir="out_high")
            self.add("delay", min_us=5)
            self.add("recv", data="DAT", clk="CLK", bits=c["db"], reply=word)
            self.add("mode", line="DAT", dir="out_low")
            self.add("clock", min_ns=1000)
        elif fam == "OTP3_1":
            self.add("send", data="PA4", clk="PA0", value=addr, bits=c["ab"])
            self.add("mode", line="PA4", dir="out_low")
            self.add("mode", line="PA7", dir="out_high")   # commit: PA7 pulse ≥ 1 µs + one PA0 clock
            self.add("delay", min_us=1)
            self.add("mode", line="PA7", dir="out_low")
            self.add("send", data="PA7", clk="PA0", value=0, bits=1)
            self.add("recv", data="DAT", clk="PA0", bits=c["db"], reply=word)
        else:
            self.add("send", data="PA4", clk="CLK", value=addr, bits=c["ab"])
            self.add("mode", line="PA4", dir="out_low")
            self.add("recv", data="DAT", clk="CLK", bits=c["db"], reply=word)


def word_at(c, addr):
    """The test memory pattern (the C test fills the fake IC with the same)."""
    return (addr * 0x1235 + 7) & ((1 << c["db"]) - 1)


def probe(name):
    """§4.6: FLASH (read command, 16-bit ACK), then OTP1_2, OTP2_2, OTP2_1, OTP3_1 (write command, ID as for 16 bits)."""
    c = CHIPS[name]
    t = Trace()
    gap = 0
    for fam in ("FLASH_1", "OTP1_2", "OTP2_2", "OTP2_1", "OTP3_1"):
        t.enter(fam, PROBE_VPP, PROBE_VDD, gap)
        if fam == "FLASH_1":
            t.command(fam, "read")
            t.ack(16, reply=None)
        else:
            t.command(fam, "write")
            t.otp_id(fam, 16, reply=None)
        t.leave()
        gap = PAUSE_LEAVE
        if fam == "FLASH_1" and c["fam"] in FLASH:
            break
        if fam == c["fam"]:
            break
    return t


def read(name, addr=0x10, count=2):
    """§4.7 READ: [OTP GetID pass] enter at the read command voltages, command [FLASH ACK], [hv], words, leave."""
    c = CHIPS[name]
    fam = c["fam"]
    vdd_c, vpp_c, vdd_h, vpp_h = c["rd"]
    t = Trace()
    gap = 0
    if fam not in FLASH:
        t.enter(fam, vpp_c, vdd_c)
        t.command(fam, "write")
        t.otp_id(fam, c["db"], reply=c["id"])
        t.leave()
        gap = PAUSE_LEAVE
    t.enter(fam, vpp_c, vdd_c, gap)
    t.command(fam, "read")
    if fam in FLASH:
        t.ack(17 if fam == "FLASH_2" else 16, reply=c["id"])
    if (vdd_h, vpp_h) != (vdd_c, vpp_c):
        t.add("vdd", mV=vdd_h, min_us=1000)
        t.add("vpp", mV=vpp_h, min_us=1000)
    for i in range(count):
        t.read_word(c, addr + i, word_at(c, addr + i))
    t.leave()
    return t


def block_words(c, aligned):
    """The words the test writes into a block: the memory pattern, one word left blank (all ones)."""
    n = c["blk"][0]
    words = [word_at(c, aligned + i) for i in range(n)]
    words[-1] = (1 << c["db"]) - 1
    return words


def write_block(t, c, aligned, words):
    """§4.9 WriteBlock of one block; the 100 µs pause after it."""
    fam = c["fam"]
    _, groups, clocks = c["blk"]
    if fam == "FLASH_2":
        t.add("send", data="DAT", clk="CLK", value=aligned, bits=c["ab"])
        for w in words:
            t.add("send", data="DAT", clk="CLK", value=w, bits=c["db"])
        t.add("mode", line="DAT", dir="in")
        for _ in range(groups):
            t.add("clock", min_ns=1000)
            t.add("delay", min_us=2)
            for _ in range(clocks):
                t.add("clock", min_ns=40000)
                t.add("delay", min_us=40)
            for _ in range(4):
                t.add("clock", min_ns=2000)
                t.add("delay", min_us=2)
    elif fam not in FLASH:
        # OTP1_2 / OTP2_x: data then address on PA4 (forced to 0 after each send), +1 clock, 4 µs, then per group CLK
        # held high with the programming pulses on PA4 (30 µs high / 30 µs low), CLK low, 4 µs, +1 clock, 4 µs; 25 µs
        for v, bits in [(w, c["db"]) for w in words] + [(aligned, c["ab"])]:
            t.add("send", data="PA4", clk="CLK", value=v, bits=bits)
            t.add("mode", line="PA4", dir="out_low")
        t.add("clock", min_ns=1000)
        t.add("delay", min_us=4)
        for _ in range(groups):
            t.add("mode", line="CLK", dir="out_high")
            for _ in range(clocks):
                t.add("mode", line="PA4", dir="out_high")
                t.add("delay", min_us=30)
                t.add("mode", line="PA4", dir="out_low")
                t.add("delay", min_us=30)
            t.add("mode", line="CLK", dir="out_low")
            t.add("delay", min_us=4)
            t.add("clock", min_ns=1000)
            t.add("delay", min_us=4)
        t.add("delay", min_us=25)
        t.add("delay", min_us=100)
        return
    else:
        for w in words:
            t.add("send", data="DAT", clk="CLK", value=w, bits=c["db"])
        t.add("send", data="DAT", clk="CLK", value=aligned, bits=c["ab"])
        t.add("mode", line="DAT", dir="in")
        t.add("delay", min_us=4)
        for _ in range(groups):
            for _ in range(clocks):
                t.add("clock", min_ns=15000)
                t.add("delay", min_us=15)
            t.add("clock", min_ns=1000)
            t.add("delay", min_us=4)
    t.add("mode", line="DAT", dir="out_low")
    t.add("delay", min_us=25)
    t.add("delay", min_us=100)


def write(name, addrs):
    """§4.9 WRITE: [OTP GetID pass at the write command voltages], enter at the write command voltages, command,
    [FLASH ACK], hv (≥ 10 ms each), blocks, leave."""
    c = CHIPS[name]
    fam = c["fam"]
    vdd_c, vpp_c, vdd_h, vpp_h = c["wr"]
    t = Trace()
    gap = 0
    if fam not in FLASH:
        t.enter(fam, vpp_c, vdd_c)
        t.command(fam, "write")
        t.otp_id(fam, c["db"], reply=c["id"])
        t.leave()
        gap = PAUSE_LEAVE
    t.enter(fam, vpp_c, vdd_c, gap)
    t.command(fam, "write")
    if fam in FLASH:
        t.ack(17 if fam == "FLASH_2" else 16, reply=c["id"])
    t.add("vdd", mV=vdd_h, min_us=10000)
    t.add("vpp", mV=vpp_h, min_us=10000)
    for a in addrs:
        write_block(t, c, a, block_words(c, a))
    t.leave()
    return t


def erase(name):
    """§4.8 ERASE: enter at the erase command voltages, command, ACK, hv (≥ 10 ms each), erase clocks, +1 clock, leave."""
    c = CHIPS[name]
    fam = c["fam"]
    vdd_c, vpp_c, vdd_h, vpp_h = c["er"]
    high_us = 40000 if fam == "FLASH_2" else 5000
    t = Trace()
    t.enter(fam, vpp_c, vdd_c)
    t.command(fam, "erase")
    t.ack(17 if fam == "FLASH_2" else 16, reply=c["id"])
    t.add("vdd", mV=vdd_h, min_us=10000)
    t.add("vpp", mV=vpp_h, min_us=10000)
    for _ in range(c["erc"]):
        t.add("mode", line="CLK", dir="out_high")
        t.add("delay", min_us=high_us)
        t.add("mode", line="CLK", dir="out_low")
        t.add("delay", min_us=1)
        t.add("mode", line="CLK", dir="out_high")
        t.add("delay", min_us=1)
        t.add("mode", line="CLK", dir="out_low")
        t.add("delay", min_us=4)
    t.add("clock", min_ns=1000)
    t.leave()
    return t


def flash_wrong_ack():
    """Safety: a FLASH IC answering another ID — no voltage above the command levels, then leave."""
    c = CHIPS["PFS154"]
    vdd_c, vpp_c, _, _ = c["rd"]
    t = Trace()
    t.enter("FLASH_1", vpp_c, vdd_c)
    t.command("FLASH_1", "read")
    t.ack(16, reply=0xAA2)
    t.leave()
    return t


def otp_wrong_id():
    """Safety: an OTP IC with another ID — the GetID pass only, no read/write session."""
    c = CHIPS["PMS150C"]
    vdd_c, vpp_c, _, _ = c["rd"]
    t = Trace()
    t.enter("OTP1_2", vpp_c, vdd_c)
    t.command("OTP1_2", "write")
    t.otp_id("OTP1_2", c["db"], reply=0xA17)
    t.leave()
    return t


def traces():
    out = {}
    for name in ("PFS154", "PFS172", "PMS150C", "PMS152", "PMS133", "PMC251"):
        fam = CHIPS[name]["fam"].lower().replace("_", "")
        out[f"{fam}_probe"] = dict(family=CHIPS[name]["fam"], chip=name, operation="probe", input={}, t=probe(name))
        out[f"{fam}_read"] = dict(family=CHIPS[name]["fam"], chip=name, operation="read",
                                  input={"addr": 0x10, "count": 2}, t=read(name))
    for name in ("PFS154", "PFS172"):
        fam = CHIPS[name]["fam"].lower().replace("_", "")
        out[f"{fam}_erase"] = dict(family=CHIPS[name]["fam"], chip=name, operation="erase", input={}, t=erase(name))
    for name in ("PFS154", "PFS172", "PMS150C", "PMS152"):
        fam = CHIPS[name]["fam"].lower().replace("_", "")
        n = CHIPS[name]["blk"][0]
        out[f"{fam}_write_block"] = dict(family=CHIPS[name]["fam"], chip=name, operation="write",
                                         input={"addrs": [0x40, 0x40 + n]}, t=write(name, [0x40, 0x40 + n]))
        fuse = CHIPS[name]["words"] - CHIPS[name]["blk"][0]   # the block holding the fuse word
        out[f"{fam}_fuse"] = dict(family=CHIPS[name]["fam"], chip=name, operation="write",
                                  input={"addrs": [fuse]}, t=write(name, [fuse]))
    out["safety_flash_wrong_ack"] = dict(family="FLASH_1", chip="PFS154", operation="read", input={"addr": 0, "count": 1,
                                         "answer_id": 0xAA2}, t=flash_wrong_ack())
    out["safety_otp_wrong_id"] = dict(family="OTP1_2", chip="PMS150C", operation="read", input={"addr": 0, "count": 1,
                                      "answer_id": 0xA17}, t=otp_wrong_id())
    return out


def render(d) -> str:
    doc = {k: v for k, v in d.items() if k != "t"}
    doc["events"] = d["t"].ev
    lines = ["{"]
    for k in ("family", "chip", "operation", "input"):
        lines.append(f'  "{k}": {json.dumps(doc[k])},')
    lines.append('  "events": [')
    lines.append(",\n".join("    " + json.dumps(e) for e in doc["events"]))
    lines.append("  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    stale = []
    files = {f"{name}.json": render(d) for name, d in traces().items()}
    if not a.check:
        OUT.mkdir(exist_ok=True)
    for fname, text in files.items():
        p = OUT / fname
        if a.check:
            if not p.exists() or p.read_text(encoding="utf-8") != text:
                stale.append(fname)
        else:
            p.write_text(text, encoding="utf-8", newline="\n")
    if a.check and stale:
        print("traces out of date: " + ", ".join(stale) + " (run gen_traces.py)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
