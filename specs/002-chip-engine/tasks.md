---

description: "Task list for the PADAUK chip engine"
---

# Tasks: PADAUK chip engine for the STM32 programmers

**Input**: Design documents from `specs/002-chip-engine/` — [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/hal-icp.md](contracts/hal-icp.md), [contracts/traces.md](contracts/traces.md),
[quickstart.md](quickstart.md)

**Tests**: mandatory by constitution VI — bit-level sequences on a fake HAL against reference traces, functional tests on a
bit-level IC model, and a real-hardware validation task per confirmed family (only the PFS154 is on hand, spec FR-017).
No user-visible strings are added (principle VII): status codes and client messages already exist.

**Organization**: by user story (spec.md). Paths are relative to the repository root; firmware paths under
`software/firmware/`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an unfinished task)
- **[Story]**: US1 … US5 from spec.md

---

## Phase 1: Setup (shared infrastructure)

**Purpose**: the engine library, its HAL header, the fake HAL, and the trace generator skeleton

- [X] T001 Add the static library `pdk_engine` built from `software/firmware/core/engine/*.c` (excluded from the `pdk_core` glob), link it into the G474 firmware instead of `platform/common/chip_engine_stub.c` (the F072 target and the stub were later removed), and keep `pdk-sim` on `platform/host/chip_model.c` in `software/firmware/CMakeLists.txt` and `software/firmware/cmake/stm32_common.cmake`
- [X] T002 [P] Create `software/firmware/hal/hal_icp.h` exactly as [contracts/hal-icp.md](contracts/hal-icp.md): `icp_line_t` (CLK, DAT, PA4, PA0, PA7), `icp_dir_t`, idle/mode/clock/send/recv/level/delay_ns/vdd_max, the calibration counter functions, with the "durations are minimums" and "≤ 100 ms per call" rules in the header comment
- [X] T003 [P] Create the fake HAL `software/firmware/tests/fakes/fake_icp.c` / `fake_icp.h`: implements `hal_icp.h`, records every call as an event (`mode`, `clock`, `send`, `recv`, `delay`, `pulse_pa4`, `calib_*`, `idle`) plus the `power_apply` set points seen through the fake power HAL, exposes the event list, and forwards line activity to a pluggable IC model; add an engine test group to `software/firmware/tests/CMakeLists.txt` that links `pdk_engine` + `fake_icp` (not `fake_chip`)
- [X] T004 [P] Create the trace generator skeleton `software/tests/vectors/gen_traces.py` (stdlib only, `--check` mode like `docs/gen_protocol.py`) writing `software/tests/vectors/traces/<family>_<operation>.json` in the format of [contracts/traces.md](contracts/traces.md), with the chip parameters taken from its own table transcribed from `docs/easy-pdk-programmer-protocol.md` §9 (not from the C table, to stay independent)
- [X] T005 [P] Add `python software/tests/vectors/gen_traces.py --check` to the Linux host job in `.github/workflows/ci.yml`

---

## Phase 2: Foundational (blocking prerequisites)

**Purpose**: timing that can be trusted, the session state machine, the family helpers, the bit-level IC model, and the GPIO
half of the G474 HAL — needed by every story

**⚠️ CRITICAL**: no user story work before this phase is complete

- [X] T006 [P] Make `hal_delay_us()` wait **at least** the requested time (count `us + 1` ticks) in `software/firmware/platform/stm32g474/hal_g474_misc.c` and `software/firmware/platform/stm32f072/hal_f072_misc.c`, and state the "at least" contract in `software/firmware/hal/hal_time.h`
- [X] T007 Move the G474 1 MHz time base from TIM2 to TIM5 (32-bit) in `software/firmware/platform/stm32g474/hal_g474_misc.c` (`g474_time_init`, `hal_time_us`, `hal_delay_us`) so TIM2 is free for counting PA3 edges on PA15 (research R9); keep the TIM2-related comment in `software/firmware/platform/stm32g474/g474.h` in sync
- [X] T008 Implement the programming session of [data-model.md](data-model.md) in `software/firmware/core/engine/engine.c` / `engine_internal.h`: states IDLE/OPEN/HV, `enter(chip, mode)` (wait the remaining post-leave pause, `hal_icp_idle`, VPP = cmd via `power_apply` with ≥ 100 µs, VDD = cmd with ≥ 500 µs, family line setup), `raise()` (VDD/VPP hv, ≥ 1 ms for read, ≥ 10 ms for write/erase, VPP 0 for FLASH_2 from the chip table), `leave()` (lines idle first, then both supplies 0 immediately, record `left_at` and the 10 ms / +100 ms pause), session reuse for the same chip and mode, `chip_engine_safe_state()` = `leave()` + `hal_power_off_all()`, the `poll()` wrapper that leaves and returns `PDK_CANCELLED`, and power faults mapped to `VDD_FAULT`/`VPP_FAULT`
- [X] T009 Implement the family helpers in `software/firmware/core/engine/family.c` / `family.h`: per-family line roles (data out/in line, clock), the command preamble (`0xA5A5A5A0|cmd` on PA6 / PA4, `0x5A5A5A50|cmd`, OTP3_1 `0x5A5A5A5A` + 27 zeros), command codes (read/write/erase per FLASH_1 / FLASH_2 / OTP), the FLASH ACK (16 / 17 bits, `ack & 0xFFF == id12`, then DAT output + one clock), and the OTP `GetID` pass (write command at command voltages, `2·data_bits+12` / `data_bits+12` / `data_bits+13` bits with the PA0 clock for OTP3_1, leave before any programming voltage) — all from `docs/easy-pdk-programmer-protocol.md` §4.3–§4.5
- [X] T010 [P] Implement the bit-level IC model `software/firmware/tests/fakes/fake_icp_ic.c` / `fake_icp_ic.h` driven by `fake_icp`: decodes the preamble and command, answers the ACK / GetID bits for a configured chip (or nothing: "no IC", or a wrong ID), serves read words, latches write blocks on the programming clocks / PA4 pulses (only 1 → 0, OTP never back to 1), erases on the erase clocks (FLASH), and supports a weak cell and a missing VPP (writes do not stick)
- [X] T011 [P] Implement the GPIO half of the G474 HAL in `software/firmware/platform/stm32g474/hal_g474_icp.c`: lines PB3 (CLK), PB5 (DAT), PC11 (PA4), PB6 (PA0), PB7 (PA7), PA15 always an input; `hal_icp_idle` (inputs, pull-down on CLK and DAT), `hal_icp_mode`, `hal_icp_clock`, `hal_icp_send` / `hal_icp_recv` MSB first with ≥ 1 µs clock-high timed by DWT `CYCCNT`, `hal_icp_level`, `hal_icp_delay_ns`, `hal_icp_vdd_max_mV` = 5000; enable DWT at start-up in `software/firmware/platform/stm32g474/system.c`
- [X] T012 [P] Unit test of the session rules in `software/firmware/tests/test_engine_session.c` (fake HAL + fake power): VPP before VDD with the minimum pauses, raise pauses per mode, `leave` order (idle before supplies off) and immediate return, the lazy post-leave pause charged to the next enter, session reuse for the same chip/mode and a new session for another mode, cancel → `CANCELLED` + safe state, a power fault → `VPP_FAULT` + safe state

**Checkpoint**: the engine skeleton enters and leaves safely; user stories can start

---

## Phase 3: User Story 1 — Identify and read a real IC (Priority: P1) 🎯 MVP

**Goal**: `probe` and `read` (and so `VERIFY`, `READ`, the `ID` stage, `COPY`'s read) work on a real PFS154 through the engine

**Independent Test**: `pdkprog probe` and `pdkprog -n PFS154 read` on a known PFS154 give the right type and the original image;
100 reads are identical (quickstart §4)

### Tests for User Story 1

- [X] T013 [P] [US1] Generate the probe, read (2 words), and GetID traces for all six families in `software/tests/vectors/gen_traces.py` (probe at VPP 4.5 V / VDD 2.0 V with the FLASH → OTP1_2 → OTP2_2 → OTP2_1 → OTP3_1 order of §4.6; read with the FLASH 5 µs pause and the OTP3_1 PA0 clock and PA7 commit) and commit `software/tests/vectors/traces/*_probe.json`, `*_read.json`, `*_getid.json`
- [X] T014 [P] [US1] Trace test `software/firmware/tests/test_engine_trace.c`: loads a trace JSON (a small reader in the test), runs the matching primitive on `fake_icp` with the model answering the trace's `reply` values, compares events per the rules of [contracts/traces.md](contracts/traces.md); cases for probe, read, and GetID of every family, plus the safety traces "FLASH wrong ACK" and "OTP wrong ID" (no voltage above the command levels, then idle and both supplies 0)
- [X] T015 [P] [US1] Functional test `software/firmware/tests/test_engine_read.c` on `fake_icp_ic`: probe returns id12 and family for each family's chip, a missing IC → `CHIP_NOT_FOUND` and a wrong ID → `CHIP_ID_MISMATCH` without any `vdd`/`vpp` above the probe levels, `chip_engine_read` streams the model memory to the sink for a range and for the whole chip, the reserved area is read as stored, cancel mid-read → `CANCELLED` + safe state, `poll` called at least every 64 words

### Implementation for User Story 1

- [X] T016 [US1] Implement `chip_engine_probe()` in `software/firmware/core/engine/probe.c`: one enter/leave per family at VPP 4.5 V / VDD 2.0 V in the §4.6 order, FLASH read-command ACK (16 bits; also the `<< 1` interpretation for the 17-bit FLASH_2 ACK), OTP GetID with `data_bits = 16` and the code-width shifts of §4.6 matched against the chip table (`chip_find_by_id`), returning `id12` and the family; empty → `CHIP_NOT_FOUND`
- [X] T017 [US1] Implement `chip_engine_read()` in `software/firmware/core/engine/engine.c` + `family.c`: OTP GetID pass first, enter at the read command voltages, command + ACK, raise to the read hv voltages (≥ 1 ms each), per address: FLASH — address bits, DAT input, CLK high ≥ 5 µs, data bits, DAT output, one clock; OTP1_2/OTP2_x — address on PA4, data from PA6; OTP3_1 — address with the PA0 clock, PA7 commit, data with the PA0 clock; `sink(addr, word & mask)`, `poll()` every 64 words; the session stays open for a following read
- [X] T018 [US1] Run the full host suite (`ctest`, `pytest software/client/tests --sim build/host/pdk-sim`) and make sure the simulator path is unchanged (it still links `chip_model.c`), fixing any build split issue from T001
- [ ] T019 [US1] Hardware validation on the G474 board with a PFS154 per quickstart §3–§4: no IC / reversed IC 20 times each (scope: never above the probe levels — SC-005), probe, read vs the known image, 100 identical reads (SC-004), logic-analyzer capture of the read preamble/ACK/clock widths; record results and captures in `specs/002-chip-engine/hw-validation.md`

**Checkpoint**: the programmer identifies and reads a real PFS154 — MVP

---

## Phase 4: User Story 2 — Program a FLASH IC (Priority: P1)

**Goal**: `PROGRAM` (erase, blank check, write, verify, fuse, final verify) writes a real PFS154 within the time budget

**Independent Test**: program a PFS154 with a test image without calibration, read it back here and with Easy PDK, run it
(quickstart §5)

### Tests for User Story 2

- [X] T020 [P] [US2] Generate the FLASH_1 and FLASH_2 erase, write_block, and fuse-write traces in `software/tests/vectors/gen_traces.py` (FLASH_1: data then address, DAT input, 4 µs, 8 × (15 µs/15 µs) + 1 clock + 4 µs, DAT output, 25 µs; FLASH_2: address then data, +1 clock, 2 µs, 4 × (40 µs/40 µs), 4 × (2 µs/2 µs), VPP 0 in the operation; erase: hv ramps ≥ 10 ms, `erase_clocks` × (CLK high 5 ms / 40 ms for FLASH_2, then the short pulses), +1 clock) and commit the JSON files
- [X] T021 [P] [US2] Extend `software/firmware/tests/test_engine_trace.c` with the FLASH erase, write_block (one block and two consecutive blocks sharing one session), and fuse-write cases, and the "cancel between two blocks" safety trace
- [X] T022 [P] [US2] Functional test `software/firmware/tests/test_engine_write.c` on `fake_icp_ic`: erase then blank, write blocks then read back equal (PFS154 and PFS172), only 1 → 0 changes, an all-ones block is not sent, a weak cell shows up on read-back, consecutive `write_block` calls reuse one session (one enter, one leave), a different primitive in between leaves and re-enters, cancel mid-erase and mid-write end safely, ID swapped between blocks → `CHIP_ID_MISMATCH`

### Implementation for User Story 2

- [X] T023 [US2] Implement `chip_engine_erase()` in `software/firmware/core/engine/engine.c` + `family.c` (FLASH only; OTP → `OP_UNSUPPORTED` without applying any voltage): enter at the erase command voltages, command + ACK, raise ≥ 10 ms each, `erase_clocks` × the §4.8 clock pattern with `poll()` between clocks (each 5 ms / 40 ms high stays < 100 ms), +1 clock, leave with the 100 ms pause
- [X] T024 [US2] Implement `chip_engine_write_block()` for FLASH_1 and FLASH_2 in `software/firmware/core/engine/engine.c` + `family.c`: open or continue a WRITE session (enter at write command voltages, command + ACK, raise ≥ 10 ms each), the §4.9 block sequence per family with `code_bits`/`addr_bits` from the chip table, ≥ 100 µs after each block, `poll()` per block, the session left at the end of the operation or on a mode change (100 ms pause)
- [ ] T025 [US2] Hardware validation per quickstart §5 with a PFS154: program the test image (no calibration), verify, read back, run on the target board; logic-analyzer capture of one write block and one erase clock; SC-008 against Easy PDK if available (else against the free-pdk build); record in `specs/002-chip-engine/hw-validation.md`

**Checkpoint**: Stories 1 and 2 give a working FLASH programmer on real silicon

---

## Phase 5: User Story 3 — Calibrate oscillators on a real IC (Priority: P2)

**Goal**: IHRC / ILRC / BandGap calibration, MEASURE_MASTER and SNAP_STANDARD on a real PFS154 with the FR-055 checks

**Independent Test**: program `helloworld_pfs154.ihx` (IHRC 8 MHz stub), measure the IC clock externally; copy with
`--ihrc measure` and check three copies (quickstart §6)

### Tests for User Story 3

- [X] T026 [P] [US3] Add a fake calibration counter to `software/firmware/tests/fakes/fake_icp.c`: a configurable curve `SYSCLK(k)` (linear, flat = "deaf", broken = never reaching the target, none = no loop), the trim value advanced by `hal_icp_calib_pulses`, edge counts for a window from the curve, a PA0 level for BandGap that flips at a configured `k` (or is not driven)
- [X] T027 [P] [US3] Unit test `software/firmware/tests/test_engine_calib.c`: IHRC search picks the closest trim on a linear curve (and the reported frequency), ILRC steps of 0x10, BandGap first `k` with PA0 = 0 after ≥ 500 µs, SNAP_STANDARD picks the nearest standard frequency within 10 % else keeps `kk`, `measure_freq` without pulses and "no signal" after 1 s, FR-055 refusals (first reading outside ¼…4 → `CALIB_WRONG_SITE`, no response to steps → `CALIB_WRONG_SITE`, PA0 not driven → `CALIB_WRONG_SITE`, target unreachable → `CALIB_FAILED` with the best value), VDD above `hal_icp_vdd_max_mV` refused, windows split into ≤ 100 ms polled steps, cancel → `CANCELLED` + safe state

### Implementation for User Story 3

- [X] T028 [US3] Implement the calibration half of the G474 HAL in `software/firmware/platform/stm32g474/hal_g474_icp.c`: SPI3 slave (SCK = PB3 ← IC PA3, MISO = PC11 → IC PA4, MSB first, CPOL 0 / CPHA 0) with a DMA transmit buffer holding exactly `n` one-bits then zeros for `hal_icp_calib_pulses` (timeout when the IC does not clock them out), TIM2 external clock mode 1 on TI1 = PA15 counting PA3 edges for `hal_icp_calib_count` with the window timed by DWT, `hal_icp_calib_begin/end` restoring the lines to idle
- [X] T029 [US3] Implement `software/firmware/core/engine/calibrate.c`: `chip_engine_calibrate()` (VDD = the stub's mV via `power_apply`, VPP 0, ≥ 1 ms start-up, the window sizes of research R9, IHRC 1…0x9F step 1 closest to the target, ILRC 0x00…0xF0 step 0x10, BandGap 0x00…0xF0 step 0x10 reading PA0 after ≥ 500 µs, SNAP_STANDARD for the `0xFF0000kk` targets as in feature 001, the FR-055 plausibility / step-response / PA0-driven checks, `SYSCLK = edges × 7 / window`), `chip_engine_measure_freq()` (no pulses, start value from the loop, "no signal" → `CALIB_FAILED` after 1 s), and the VDD limit check against `hal_icp_vdd_max_mV()`
- [ ] T030 [US3] Hardware validation per quickstart §6 with PFS154s: IHRC 8 MHz against a frequency counter (SC-003), ILRC and BandGap stubs, `copy --ihrc measure` with three copies within 1 % and the master bit-identical (SC-002), and SC-001 timing of a full `write` with the IHRC stub; record in `specs/002-chip-engine/hw-validation.md`

**Checkpoint**: calibrated programming and copying work on real silicon

---

## Phase 6: User Story 4 — Program an OTP IC (Priority: P2)

**Goal**: OTP write sessions (GetID before every session, PA4 programming pulses) for OTP1_2 / OTP2_2, verified by traces and the
bit model; documented as not yet confirmed on hardware (no OTP IC on hand)

**Independent Test**: on the bit model — write, read back, interrupted write + resume through the feature-001 `PROGRAM`
pipeline; on hardware when a PMS150C is available

### Tests for User Story 4

- [X] T031 [P] [US4] Generate the OTP1_2 and OTP2_2 write_block and fuse-write traces in `software/tests/vectors/gen_traces.py` (GetID pass first; data then address on PA4; +1 clock; 4 µs; CLK held high; 8 × (PA4 30 µs high / 30 µs low); CLK low; 4 µs; +1 clock; 4 µs; 25 µs) and commit them; extend `software/firmware/tests/test_engine_trace.c` with these cases
- [X] T032 [P] [US4] Functional test `software/firmware/tests/test_engine_otp.c` on `fake_icp_ic`: PMS150C and PMS152 write + read back, bits never return to 1, GetID before every session (a swapped IC is refused before the write command), erase refused without any voltage, a write interrupted after N blocks then finished by a second session

### Implementation for User Story 4

- [X] T033 [US4] Implement the OTP write-block sequence (OTP1_2 / OTP2_1 / OTP2_2) in `software/firmware/core/engine/family.c` and the GetID-guarded write session in `software/firmware/core/engine/engine.c`; OTP3_1 write → `OP_UNSUPPORTED` (read-only, as the chip table says)
- [X] T034 [US4] Document the OTP families as "implemented, trace- and model-tested, not confirmed on hardware" in `specs/002-chip-engine/hw-validation.md` and in `software/firmware/README.md` (engine section)

**Checkpoint**: the OTP path is complete in software and waits for a real IC

---

## Phase 7: User Story 5 — Run the IC with the UART bridge (Priority: P3)

**Goal**: `RUN` applies VDD only, releases the programming lines for the UART bridge, and returns to the safe state

**Independent Test**: `pdkprog start -r 3.3` with a firmware that sends `U` + text (quickstart §7)

- [X] T035 [P] [US5] Unit test `software/firmware/tests/test_engine_run.c`: `chip_engine_run_start(mV)` leaves any open session, applies VDD only (VPP 0) through `power_apply`, leaves the ICP lines idle, refuses VDD above `hal_icp_vdd_max_mV()`; `chip_engine_run_stop()` → safe state
- [X] T036 [US5] Implement `chip_engine_run_start()` / `chip_engine_run_stop()` in `software/firmware/core/engine/engine.c` (session mode RUN), consistent with `hal_uart_vdd_max_mV()` of feature 001
- [ ] T037 [US5] Hardware validation per quickstart §7 (bridge text and echo, VDD off on Esc and on USB unplug); record in `specs/002-chip-engine/hw-validation.md`

---

## Phase 8: User Story 6 — removed

The engine targets only the WeAct STM32G474 board (clarification 2026-11-26): the STM32F072 (Easy PDK) target was removed
from the project together with the stub engine (constitution 1.5.0), so the former T038–T040 are dropped.

---

## Phase 9: Polish & cross-cutting concerns

- [X] T041 [P] Update `software/firmware/platform/stm32g474/README.md` (the ICP lines, SPI3, TIM2/PA15 and TIM5 are now used; remove "not yet used by the firmware") and `software/firmware/README.md` (the engine, the confirmed families table, the trace generator)
- [X] T042 [P] Record the engine notes in the internal-interface contract `specs/001-pc-programmer-protocol/contracts/chip-engine.md` (session continuation across primitives, the lazy post-leave pause, the VDD limit for IC-driven lines) and regenerate `docs/protocol.md` with `python docs/gen_protocol.py`
- [X] T043 [P] Remove the "chip engine is a stub" notes from `specs/001-pc-programmer-protocol/plan.md` / `research.md` R19 references where they now point to this feature's results, and mark SC-004 / SC-009 of feature 001 as confirmed (or not) on hardware with a link to `specs/002-chip-engine/hw-validation.md`
- [X] T044 Run the complete validation: `ctest`, `pytest software/client/tests --sim build/host/pdk-sim`, `gen_traces.py --check`, `gen_protocol.py --check`, the G474 build with its size check; fix anything red
- [ ] T045 Write `specs/002-chip-engine/hw-validation.md` as the final record: per success criterion (SC-001 … SC-008) the measured result, the instrument, and the date; per family "confirmed on hardware" / "trace- and model-tested only"

---

## Dependencies & Execution Order

### Phase dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: depends on Setup; blocks every story
- **US1 (Phase 3)**: after Foundational — the MVP
- **US2 (Phase 4)**: after US1 (writing reuses the session, ACK, and read-back of US1)
- **US3 (Phase 5)**: after US2 (calibration writes the loop and finishes the stub through `write_block`)
- **US4 (Phase 6)**: after US2 (shares the write-session machinery); independent of US3
- **US5 (Phase 7)**: after Foundational; independent of US1–US4
- **Polish (Phase 9)**: after the stories being delivered

### Within each story

- trace generation and tests first (they must fail before the implementation), then the implementation, then hardware validation
- `family.c` helpers before the primitives that use them; the HAL half needed by a story before its hardware validation

### Parallel opportunities

- Setup: T002, T003, T004, T005 in parallel after T001
- Foundational: T006, T010, T011, T012 in parallel; T007 before T011's hardware use; T008 → T009
- In each story the trace generation, the trace test, and the functional test (marked [P]) run in parallel
- US4 and US5 can proceed in parallel with US3 once US2 is done

## Parallel example: User Story 1

```text
T013 gen_traces.py: probe / read / GetID traces for all families
T014 test_engine_trace.c: probe / read / GetID + safety traces
T015 test_engine_read.c: probe and read on the bit model
→ then T016 (probe.c), T017 (read), T018 (host suite), T019 (hardware)
```

## Implementation Strategy

### MVP first (User Story 1)

1. Phase 1 + Phase 2 (engine skeleton, HAL GPIO half on the G474, bit model)
2. Phase 3: probe and read on a real PFS154 — the safest first contact with silicon
3. **Stop and validate** on hardware (SC-004, SC-005)

### Incremental delivery

1. US1 → read-only programmer (identify, read, verify, read part of copy)
2. US2 → FLASH programmer
3. US3 → calibrated programming and MEASURE_MASTER copies (SC-001, SC-002, SC-003)
4. US4 → OTP in software (hardware when a part is available)
5. US5 → run on silicon
6. Polish → documentation and the final hardware record
