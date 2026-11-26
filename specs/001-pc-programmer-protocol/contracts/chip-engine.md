# Contract: internal interfaces core ⇄ IC engine ⇄ power

An internal (non-network) contract between the platform-independent firmware core, the IC engine
(the PADAUK implementation is feature `002-chip-engine`; in this feature — a model for the simulator),
and the power hardware. It fixes the guarantees of constitution principle III and FR-040.

## 1. `hal_power.h` — power hardware (implemented by the platform)

| Function | Description |
|---|---|
| `power_set(rail, mV)` | set VDD or VPP; `0` — switch off with active discharge |
| `power_measure(rail) → mV` | the measured rail voltage (averaging is the platform's responsibility) |
| `power_off_all()` | immediately VDD = VPP = 0, discharge on; safe to call from any context, including the fault handler |
| `power_limits() → {vdd_max_mV, vpp_max_mV}` | the hardware limits of the supplies |

The platform MUST have a hardware maximum-voltage limit that is independent of the firmware (constitution, III).

## 2. `core/power` — the settling policy (implemented by the core)

`power_apply(rail, mV, min_pause_us) → OK | VDD_FAULT | VPP_FAULT` — the rule from
[protocol.md §6.1](protocol.md): the minimum pause **and** 3 consecutive measurements within ±2 % / ±30 mV, a 20 ms
timeout; on failure it itself calls `power_off_all()` and fills in `{rail, set_mV, meas_mV}` for `RESULT`.

The engine MUST change voltages **only** through `power_apply` (never directly through `hal_power`).

## 3. `chip_engine.h` — the IC engine

| Primitive | Description |
|---|---|
| `probe(&id, &family)` | identify the IC at low voltages (VPP 4.5 V, VDD 2.0 V) |
| `read(chip, addr, count, sink)` | read words into a sink (a flash slot / sums / comparison) |
| `erase(chip)` | erase FLASH |
| `blank_check(chip, range, excl)` | check for blank |
| `write_block(chip, addr, words)` | write one block (the block size is from the table) |
| `calibrate(chip, site, target_hz, mV) → {value, measured_hz}` | calibration with a measurement loop; checks that exactly this stub is active (FR-055) → otherwise `CALIB_WRONG_SITE` |
| `measure_freq(chip, site, start_value, mV) → hz` | frequency measurement without pulses on PA4 (R9, `MEASURE_MASTER`) |
| `run_start(vdd_mV)` / `run_stop()` | run mode with a UART bridge |
| `safe_state()` | VDD = VPP = 0 through `power_off_all()`, IC lines Hi-Z |

Guarantees (verified by unit tests on a fake `hal_power` and an IC model):

1. **ID before HV**: any primitive that applies write/erase voltages first checks the IC ID
   (FR-020); a mismatch → `CHIP_ID_MISMATCH` without applying HV.
2. **Voltages confirmed**: no bit is passed to the IC until the last voltage change has passed
   `power_apply` (FR-040, SC-011).
3. **Safe exit**: every primitive on any exit path (success, error, cancellation through
   `poll`) leaves the IC in `safe_state()` unless the next step of the operation explicitly continues the session.
4. **Atomic steps < 100 ms**: no more than 100 ms passes between `poll()` calls (the longest step is the
   FLASH_2 erase pulse, 40 ms; voltage settling takes up to 20 ms), so that FR-024 is met.
5. **Cancellation**: `poll()` returns "cancel" → the primitive ends with `CANCELLED` after
   `safe_state()`; the exception is the `MEASURE`/`RESTORE` stages of the `COPY` operation (data-model `CopyJob`).
6. **The same stub** (FR-055, `docs/padauk-calibration.md` §8): before the first "+1" pulse `calibrate`
   measures the frequency and compares it with the one expected for the stub type (`target_hz / 7` on PA3; tolerance 1/4…4); during
   the search it watches that the frequency changes monotonically with the step; for BG it checks that PA0 is driven
   by the IC (switching a weak pull-up/pull-down, the level must not follow it). A violation →
   `CALIB_WRONG_SITE` with no further pulse and no write.

### 3.1 Engine notes (the PADAUK implementation, feature `002-chip-engine`)

- **Session continuation.** Consecutive calls of the same primitive with the same mode on the same IC (`write_block`
  block after block, `read` chunk after chunk) continue one programming session: the ID check and the voltage ramps
  happen once. Any other primitive, `safe_state()` or a cancellation leaves the session first. An OTP session always
  starts with its own GetID pass.
- **Lazy post-leave pause.** Leaving the mode removes VDD and VPP at once (the safe state is immediate); the mandatory
  free-pdk pause after leaving (10 ms; +100 ms after erase/write; +40 ms after calibration) is waited for by the **next**
  enter, with `poll()` called every millisecond, so a cancel during the pause is still honoured.
- **VDD limit for the lines the IC drives.** In calibration and in `run_start` the IC drives the programmer's inputs
  (PA3, PA0, the UART lines) at its VDD. `hal_icp_vdd_max_mV()` (G474: 5.0 V, PB6 is FT_c) caps that VDD: a higher
  request is refused with `OP_UNSUPPORTED` before any voltage. Programming itself is not limited (the programmer drives
  the lines then).
- **VDD only.** Calibration, `measure_freq` and `run_start` apply VDD alone (VPP stays 0): the IC runs its program
  instead of entering programming mode.

## 4. The controls platform (local start)

The core receives only the events `local_start(request)` and `local_cancel()` from the platform and reports the indication state
`ui_state(state, detail)`; the mapping to buttons, switches, a display, and handler signals is
a separate feature (research R16). The set is fixed (FR-054):

| `request` | Operation | Note |
|---|---|---|
| `PROGRAM` | `PROGRAM` of the active package | the R16 priorities may replace it with `SELF_TEST` / `RESTORE_MASTER` |
| `COPY` | `COPY` with the `cfg_copy_*` modes | the same |
| `VERIFY` | `VERIFY` of the connected IC against the active package | |
| `SELF_TEST` | `SELF_TEST` | an explicit operator request |
| `RESTORE_MASTER` | `RESTORE_MASTER` | only with `MASTER_RESTORE_PENDING`, otherwise ignored |
| `local_cancel()` | `OP_CANCEL` of the current local operation | as from a PC; for `COPY` in `MEASURE` — restore first |

`ui_state.state`: `READY`, `BUSY` (`detail` = stage and percent), `SUCCESS`, `FAIL` (`detail` = class:
`STORAGE`, `NO_PACKAGE`, `CHIP`, `CALIB`, `POWER_SELFTEST`, `MASTER_PENDING` + the status code), `BLOCKED`
(local programming is replaced by the self-test). The WeAct STM32G474 board: one button (KEY) →
`local_start(PROGRAM)`, one LED — a simplified display of the states.
