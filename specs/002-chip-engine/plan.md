# Implementation Plan: PADAUK chip engine for the STM32 programmers

**Branch**: `002-chip-engine` | **Date**: 2026-11-26 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/002-chip-engine/spec.md`

## Summary

Replace the chip-engine stub of the STM32 firmwares with a real PADAUK engine, so that every operation of feature 001
(probe, read, program, verify, copy with calibration restore, patch, run) works on real ICs without touching the protocol,
the client, or the pipelines. The engine is portable C in the core (`core/engine/`, library `pdk_engine`) implementing the
existing `chip_engine.h`; it ports the free-pdk 1.3 family sequences (FLASH_1, FLASH_2, OTP1_2, OTP2_1, OTP2_2, OTP3_1
read-only) over a new narrow HAL `hal_icp.h` (timed bit shifting on the IC lines, the calibration counter and "+1" pulses).
Sessions stay open across consecutive primitives of the same mode to meet the 10 s budget, every voltage goes through
`power_apply`, the ID is checked before any programming voltage, and every exit is the safe state. Correctness is shown
without hardware by reference traces from an independent generator and by a unit-level bit model of the IC; on hardware the
PFS154 is confirmed on the WeAct STM32G474 board (the only IC on hand), other families stay "trace-tested, not confirmed".

## Technical Context

**Language/Version**: C11 (firmware, `arm-none-eabi-gcc` ≥ 12; host gcc/clang, MinGW-w64 on Windows); Python ≥ 3.8 for the
trace generator and the existing client tests

**Primary Dependencies**: none new; CMSIS device headers for the STM32 HAL implementations (already fetched); the free-pdk
sources as a reference only (constitution)

**Storage**: N/A (the engine keeps only RAM state: the open session, DMA buffers)

**Testing**: CTest unit tests on the host with a fake `hal_icp` (`tests/test_engine_*.c`): trace comparison against
`software/tests/vectors/traces/*.json` from `gen_traces.py`, functional tests against a bit-level IC model
(`tests/fakes/fake_icp_ic.c`); the existing pytest contract suite keeps running against `pdk-sim` (unchanged model engine);
hardware validation per [quickstart.md](quickstart.md) with a logic analyzer, an oscilloscope, a frequency counter

**Target Platform**: WeAct STM32G474CEU6 + `hardware/stm32g474-board` — the only firmware hardware target (PFS154 confirmed
on hardware; the STM32F072 target was removed, constitution 1.5.0); host — trace and model tests

**Project Type**: embedded firmware (core library + platform HAL)

**Performance Goals**: full PFS154 programming with one IHRC calibration ≤ 10 s (SC-001; estimate ≈ 3–4 s, research R11); a
full read of the largest IC (4096 words) ≤ 0.5 s

**Constraints**: documented free-pdk timings as minimums; no atomic step > 100 ms without `poll()`; G474 firmware within
bank 1 (size check); programmer pins never driven above their rating (G474 PB6 ≤ 5.0 V); no write/erase
voltage before the ID check; safe state on every exit

**Scale/Scope**: 6 protocol families, 13 ICs in the chip table (7 writable); hardware confirmation for PFS154 (FLASH_1) only

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Check | Status |
|---|---|---|
| I. Smart programmer, thin client | All IC knowledge (sequences, timings, calibration search) is in the firmware engine; client and protocol unchanged | ✅ |
| II. Standalone operation | The engine serves the local `PROGRAM` / `COPY` / `VERIFY` of feature 001 the same way; no PC dependency added | ✅ |
| III. Chip and power safety | ID before HV (R5, two layers), every voltage via `power_apply` with documented minimum pauses (R6), safe state on every exit, lazy post-leave pause keeps `safe_state()` instant (R4), pin voltage limits (R10); OTP rules of feature 001 unchanged | ✅ |
| IV. Hardware abstraction | Algorithms in `core/engine` (portable C); hardware only via `hal_icp.h` + `core/power` + `hal_time.h`; another board = its HAL (R1, R2) | ✅ |
| V. Versioned protocol | No protocol change; `docs/protocol.md` regenerated only if the internal-interface contract (chip-engine) gains notes | ✅ |
| VI. Verifiability | Fake-HAL trace tests against an independent reference generator (R7), bit-model functional tests (R8); PFS154 confirmed on a real IC, other families explicitly marked "not confirmed on hardware" (spec FR-017) | ✅ (with the documented hardware limit) |
| VII. English-first, localized UI | All new code, comments, specs in English; no new user-visible strings (status codes already exist; client unchanged) | ✅ |

No violations; Complexity Tracking stays empty.

**Post-design re-check (after Phase 1)**: the contracts ([contracts/hal-icp.md](contracts/hal-icp.md),
[contracts/traces.md](contracts/traces.md)) keep all hardware access behind `hal_icp.h`, the session state machine in
[data-model.md](data-model.md) enforces the safe exit and ID-before-HV rules, and no protocol or UI change appears. ✅

## Project Structure

### Documentation (this feature)

```text
specs/002-chip-engine/
├── plan.md              # This file
├── research.md          # Phase 0: decisions R1–R13
├── data-model.md        # Phase 1: session, family sequence, calibration measurement
├── quickstart.md        # Phase 1: build, tests, hardware validation steps
├── contracts/
│   ├── hal-icp.md       # the new HAL interface for the IC lines and the calibration counter
│   └── traces.md        # reference trace format and coverage
├── checklists/requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks)
```

### Source Code (repository root)

```text
software/firmware/
├── hal/
│   └── hal_icp.h                  # NEW: IC lines, timed bit shifting, calibration counter / pulses, pin limits
├── core/engine/                   # NEW library pdk_engine (implements hal/chip_engine.h)
│   ├── engine.c                   # primitives, session state machine, safe state, cancel/poll
│   ├── family.c / family.h        # per-family sequences: enter/leave, preamble, ACK/GetID, read word, write block, erase
│   ├── probe.c                    # low-voltage auto-detection (free-pdk §4.6)
│   └── calibrate.c                # IHRC/ILRC/BG search, FR-055 checks, measure_freq, snap
├── platform/
│   ├── stm32g474/hal_g474_icp.c   # NEW: GPIO bit shifting (DWT timing), SPI3 slave + DMA pulses, TIM2 on PA15 counting
│   ├── stm32g474/hal_g474_misc.c  # time base TIM2 → TIM5; hal_delay_us "at least"
│   ├── stm32f072/                 # REMOVED with its stub engine (platform/common/chip_engine_stub.c)
├── tests/
│   ├── fakes/fake_icp.c           # NEW: fake hal_icp recording events (trace) + driving the IC model
│   ├── fakes/fake_icp_ic.c        # NEW: bit-level IC model (FLASH/OTP semantics, ACK/ID, pulses, f(k) curve)
│   ├── test_engine_trace.c        # NEW: events vs software/tests/vectors/traces/*.json
│   ├── test_engine_ops.c          # NEW: write/read back, erase, ID mismatch, no IC, cancel, sessions, safe state
│   └── test_engine_calib.c        # NEW: IHRC/ILRC/BG search, FR-055 refusals, measure_freq, snap
└── CMakeLists.txt                 # pdk_engine library; MCU targets link it; engine tests link fake_icp

software/tests/vectors/
├── gen_traces.py                  # NEW: independent reference trace generator (from docs/easy-pdk-programmer-protocol.md)
└── traces/*.json                  # NEW: generated reference traces

specs/002-chip-engine/hw-validation.md   # hardware results (PFS154 on the G474)
```

**Structure Decision**: the engine joins the existing firmware tree as a core library (`core/engine/` → `pdk_engine`) with one
new HAL header and one HAL implementation per MCU platform; the host simulator keeps its IC model as the engine, so the
feature-001 contract suite is unaffected. Tests follow the existing CTest + fakes layout, with the reference traces next to
the other vectors in `software/tests/vectors/`.

## Complexity Tracking

> No constitution violations to justify.
