# Contract: reference traces for the family sequences

Constitution VI: "bit-level sequences for each MCU family MUST be verified by tests on a fake HAL against reference traces
derived from the documented timings". The traces are produced by `software/tests/vectors/gen_traces.py` — written from
`docs/easy-pdk-programmer-protocol.md` §4 independently of the engine's C code — and stored as JSON in
`software/tests/vectors/traces/`. The unit test `tests/test_engine_trace.c` runs each engine primitive on the fake HAL and
compares the recorded events with the trace.

## File format

One file per (family, operation): `<family>_<operation>.json`, e.g. `flash1_write_block.json`.

```json
{
  "family": "FLASH_1",
  "chip": "PFS154",
  "operation": "write_block",
  "input": {"addr": 64, "words": [12288, 12289, 16383, 16383]},
  "events": [
    {"e": "vpp", "mV": 5500, "min_us": 100},
    {"e": "vdd", "mV": 2500, "min_us": 500},
    {"e": "mode", "line": "DAT", "dir": "out_low"},
    {"e": "send", "data": "DAT", "clk": "CLK", "value": 2779096487, "bits": 32},
    {"e": "mode", "line": "DAT", "dir": "in"},
    {"e": "recv", "data": "DAT", "clk": "CLK", "bits": 16, "reply": 2721},
    {"e": "mode", "line": "DAT", "dir": "out_low"},
    {"e": "clock", "min_ns": 1000},
    {"e": "vdd", "mV": 5800, "min_us": 10000},
    {"e": "vpp", "mV": 8500, "min_us": 10000},
    {"e": "send", "data": "DAT", "clk": "CLK", "value": 12288, "bits": 14},
    "…"
  ]
}
```

| Event | Fields | Compared |
|---|---|---|
| `vdd`, `vpp` | `mV`, `min_us` (the pause after the change), `gap_min_us` (the pause since the supplies were last switched off — the post-leave pause) | set point exactly; the recorded pauses ≥ the minimums |
| `mode` | `line`, `dir` | exactly |
| `send` | `data`, `clk`, `value`, `bits` | exactly |
| `recv` | `data`, `clk`, `bits`; `reply` = what the fake IC answers (not compared) | line and bit count exactly |
| `clock` | `min_ns` (high time) | recorded high ≥ `min_ns` |
| `delay` | `min_us` | recorded ≥ `min_us`; consecutive delays may be merged |
| `idle` | — | present where the sequence leaves the session |

Merging rule: on both sides, consecutive `recv` events on the same lines are one receive (summed bits), consecutive
`delay` events one delay (summed duration), consecutive `idle` events one idle — how the engine splits them is not part of
the protocol. Entering programming mode starts with `idle` (all lines released) and `CLK` output low; a supply that is
already 0 (PFS172 VPP in operation) produces no event when the session is left. The OTP programming pulses are `mode`
events on `PA4` with `delay` minimums while `CLK` is held high.

## Coverage

| Family | probe | read (2 words) | erase | write_block | fuse write | GetID |
|---|---|---|---|---|---|---|
| FLASH_1 (PFS154, PFS173) | ✓ | ✓ | ✓ | ✓ | ✓ | — |
| FLASH_2 (PFS172, VPP 0 in operation) | ✓ | ✓ | ✓ | ✓ | ✓ | — |
| OTP1_2 (PMS150C) | ✓ | ✓ | — | ✓ | ✓ | ✓ |
| OTP2_1 (PMS133, read-only) | ✓ | ✓ | — | — | — | ✓ |
| OTP2_2 (PMS152, PMS171B) | ✓ | ✓ | — | ✓ | ✓ | ✓ |
| OTP3_1 (PMC251, read-only) | ✓ | ✓ | — | — | — | ✓ |

Plus the safety traces: a FLASH command with a wrong ACK (no `vdd`/`vpp` above the command levels follows, then `idle` and
both supplies 0), an OTP GetID with the wrong ID (no write command follows), and a cancel between two write blocks.
