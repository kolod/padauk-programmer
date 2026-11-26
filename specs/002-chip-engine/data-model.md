# Data Model: PADAUK chip engine

The engine keeps no persistent data: everything below lives in RAM for the duration of a primitive or of one programming
session. Per-IC parameters come from the chip table of feature 001 (`chip_info_t`); nothing is duplicated (FR-003).

## Programming session

The state of the IC lines and supplies between entering and leaving programming mode of one IC.

| Field | Meaning |
|---|---|
| `state` | `IDLE` (safe state), `OPEN` (mode entered, command sent, ID confirmed), `HV` (operation voltages applied) |
| `chip` | the `chip_info_t` the session was opened for |
| `mode` | `READ`, `WRITE`, `ERASE`, `GETID`, `CALIBRATE`, `RUN` |
| `vdd_mV`, `vpp_mV` | the set points currently applied (confirmed by `power_apply`) |
| `left_at_us`, `pause_us` | when the last session was left and the mandatory pause before the next enter (10 ms, +100 ms after erase/write) |

### State transitions

```
          enter(chip, mode): wait remaining pause → lines idle → VPP=cmd (≥100 µs) → VDD=cmd (≥500 µs)
 IDLE ─────────────────────────────────────────────────────────────────────────────────────────────► OPEN
   ▲      command preamble; FLASH: ACK & 0xFFF == chip.id12 / OTP: GetID pass done before enter    │
   │      mismatch → CHIP_ID_MISMATCH, no answer → CHIP_NOT_FOUND ─────────────────────────────────┤
   │                                                                                               ▼
   │      raise(mode): VDD=hv, VPP=hv (≥1 ms read / ≥10 ms write, erase; PFS172 VPP=0)            HV
   │                                                                                               │
   │      same chip + same mode requested by the next primitive → stay in HV (session continues)   │
   │                                                                                               │
   └──────── leave: lines Hi-Z → VDD=0, VPP=0 (immediately) → left_at = now, pause = 10 ms (+100 ms) ◄┘
             on: other mode/chip, safe_state(), error, cancel, power fault, end of operation
```

Rules:

- No transition into `HV` or into a write / erase command without the ID check of the same session (FR-004).
- Every error path ends in `leave` (FR-006); `leave` never waits — the pause is charged to the next `enter`.
- `poll()` is called at least every 100 ms inside `OPEN`/`HV` (between words, blocks, erase clocks, calibration windows);
  `false` → `leave`, `CANCELLED` (FR-007).

## Family sequence

The recipe of one protocol family (`chip_info_t.family`), parametrised by the chip table.

| Field | FLASH_1 | FLASH_2 | OTP1_2 | OTP2_1 / OTP2_2 | OTP3_1 |
|---|---|---|---|---|---|
| data line to the IC | PA6 | PA6 | PA4 | PA4 | PA4 (+PA0 clock) |
| data line from the IC | PA6 | PA6 | PA6 | PA6 | PA6 (PA0 clock) |
| preamble | `0xA5A5A5A0 \| cmd` | same | `0xA5A5A5A0 \| cmd` | `0x5A5A5A50 \| cmd` | `0x5A5A5A5A` + 27 zeros |
| ID | 16-bit ACK | 17-bit ACK | GetID: `2·data_bits + 12` bits | GetID: `data_bits + 12` / `2·data_bits + 12` | GetID: `data_bits + 13`, PA0 clock |
| read / write / erase cmd | 6 / 7 / 3 | C / 7 / 5 | 6 / 7 / — | 6 / 7 / — | read only |
| write block | data, then address; 8 × (15 µs / 15 µs) clocks | address, then data; 4 × (40 µs / 40 µs) | data, then address; CLK held, 8 × (30 µs / 30 µs) on PA4 | as OTP1_2 | — |

Exact sequences and timings: `docs/easy-pdk-programmer-protocol.md` §4; their encoding as reference traces:
[contracts/traces.md](contracts/traces.md).

## Calibration measurement

| Field | Meaning |
|---|---|
| `kind` | IHRC, ILRC, BG |
| `target_hz` / `mV` | from the placeholder (feature 001 `calib_site_t`); SNAP tag `0xFF0000kk` handled as in feature 001 |
| `window_us` | the counting window for one point, from the expected PA3 frequency (≤ 0.05 % per count), split into polled steps ≤ 100 ms |
| `edges`, `elapsed_us` | PA3 edges counted over the window and its measured length |
| `sysclk_hz` | `edges × 7 × 10⁶ / elapsed_us` (the loop is 7 clocks per turn) |
| `trim` | the current value of the trim register in the IC (starts at 0, each "+1" pulse adds 1) |

Validation (feature 001 FR-055, research R9): the first reading lies within ¼…4 of the expected loop frequency; the readings
change monotonically with the trim steps (within measurement noise); for BandGap PA0 must be driven by the IC; no edges for
1 s → "no signal" (MEASURE_MASTER) / `CALIB_FAILED`.
