# Implementation Plan: PC ⇄ programmer exchange protocol

**Branch**: `001-pc-programmer-protocol` | **Date**: 2026-10-09 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/001-pc-programmer-protocol/spec.md`

## Summary

A binary protocol over USB CDC: COBS frames with CRC-32 and sequence numbers, stop-and-wait with
retry and a response cache. The PC sends a single **firmware package** (a 128 B header + words + a mask of defined
words + an end-to-end CRC-32), which the programmer checks against its own IC table and atomically stores in
one of two flash slots; the active slot, settings, and statistics are recorded in a state log
with its own CRC. IC operations (`PROBE`, `READ`, `ERASE`, `BLANK_CHECK`, `PROGRAM`, `VERIFY`,
`COPY`, `RUN`) are started with a single command; the programmer itself performs all the steps and sends progress events
and a summary. The core is platform-independent C with a HAL; it is built for STM32F072, STM32G474 (a candidate for the primary MCU), and for the PC
(a simulator for tests). Deviations of the implementation from the decisions are in research R19.
The reference client is Python with a CLI in the style of `easypdkprog`. `COPY` recognizes calibration traces in the reference
dump and restores the calibration stubs; the target is either measured on a temporarily rewritten FLASH reference
(with the dump saved first and a guaranteed restore), given from the PC, rounded to a standard
frequency, or the value is carried over. `PATCH` appends individual words to an already programmed IC only after
checking the checksum of the whole firmware and the "only 1→0" rule. Every VDD/VPP change is confirmed
by the ADC (the free-pdk minimum pause **and** ±2 % / ±30 mV, a 20 ms timeout) — a common core policy
(`core/power`) for the engine and `SELF_TEST`; the self-test without an IC runs at every start.
Local actions (buttons, switches, a handler — a separate feature) arrive in the core as an abstract
`local_start`. Calibration is skipped and copied separately for IHRC/ILRC/BG; a failed calibration
writes nothing more, and an interrupted or failed OTP write can be retried if what was written is compatible with the image
(R17). The decisions are justified in [research.md](research.md).

## Technical Context

**Language/Version**: firmware — C11 (`arm-none-eabi-gcc` ≥ 12; host — gcc/clang/MSVC); client — Python ≥ 3.8 (the last one supporting Windows 7)

**Primary Dependencies**: TinyUSB (MIT) — CDC ACM; STM32 LL/CMSIS only in the platform layer; client — `pyserial`

**Storage**: the MCU's internal flash: 2 package slots × 8 KB + a state log 2 × 2 KB (the layout is set by the platform)

**Testing**: core — C unit tests (CMake/CTest, a minimal built-in framework or Unity);
client and contracts — `pytest` against the `pdk-sim` simulator; acceptance — on hardware

**Target Platform**: primary — an STM32 with higher resources (≥ 128 KB flash, ≥ 32 KB RAM, USB FS, DAC/ADC,
preferably RNG and CRC), **the model is chosen by the next specification** together with the flash layout and the number of
slots; in this feature — the **debug target STM32F072C8T6** (Easy PDK boards), the primary MCU candidate
**STM32G474CEU6** (a WeAct board + `hardware/stm32g474-board`), and the host (the simulator, the client: Windows / Linux / macOS)

**Project Type**: embedded firmware + a library/CLI on the PC

**Performance Goals**: a response to every request ≤ 100 ms; uploading a `max_package_size` (8 KB) package ≤ 3 s; progress ≥ 1 time/s; voltage settling ≤ 20 ms

**Constraints**: for the F072 debug target the firmware ≤ 44 KB flash (some features MAY be disabled by the build); for the primary MCU its specification decides; RAM without an image buffer (on-the-fly transformation from flash);
the longest atomic engine step < 100 ms; a USB drop does not interrupt an operation (except `RUN`), all data is in the programmer before the start; hardware requirements: VDD/VPP sources with active discharge to 0 V, settling < 20 ms under the load of the target board (pogo), an ADC with error < ±1 %, a hardware maximum-voltage limit

**Scale/Scope**: one client; one package; ≤ 4096 words for reading (`max_read_words`), a write package ≤ 8 KB (`max_package_size`; the largest writable in the table is 3072 words);
13 IC types in the table (from free-pdk 1.3)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle / constraint | How it is met | Before design | After design |
|---|---|---|---|
| I. Smart programmer, thin client | the package has no voltages/timings (FR-005); fuse, calibration, serial number, and validation are in the firmware (R8); the client only parses HEX and shows `LIST_CHIPS` | ✅ | ✅ |
| II. Standalone operation | the package and state in flash with CRC; a check before every write; `COPY` without a PC; random serial numbers without coordination (R18) | ✅ | ✅ |
| III. Safety (NON-NEGOTIABLE) | any DONE goes through the engine's safe state; the ID is checked before HV in the `chip_engine` contract; a USB drop does not interrupt an operation (the data is uploaded before the start; `RUN` stops safely); a failed calibration — nothing more is written; an OTP write retry only after a compatibility check "only 1→0" against the effective image before HV is applied (documented appending); the protection fuse only with `ALLOW_SECURITY_FUSE`; every VDD/VPP change goes through `power_apply` with ADC confirmation (FR-040, contracts/chip-engine.md); the automatic self-test at start blocks local programming until it succeeds (FR-041); the `VDD_FAULT`/`VPP_FAULT` codes with measured values; for OTP the blank check is mandatory (`PKG_OTP_BLANK_CHECK_REQUIRED`), appending only via `PATCH` with an "only 1→0" check before writing; the reference is modified only with `ALLOW_MODIFY_MASTER`, after the dump is committed, with a restore and verification and a persistent `MASTER_RESTORE_PENDING` flag | ✅ | ✅ |
| IV. Hardware abstraction | the core has no MCU registers; HAL: flash, CRC, transport, power (`hal_power`), controls (only `local_start`/`ui_state`), `chip_engine`; TinyUSB is portable; the flash layout comes from the platform; the `PLATFORM=host` build | ✅ | ✅ |
| V. Versioned protocol | CDC, explicit length, a CRC on every frame, `HELLO` with versions, progress, error codes; contract → `docs/protocol.md` | ✅ | ✅ |
| VI. Verifiability | core unit tests on the PC; contract tests with fault and power-cut injection on the simulator; hardware validation in the quickstart | ✅ | ✅ |
| Constraint: C11, arm-none-eabi-gcc, CMake, no IDE | yes | ✅ | ✅ |
| Constraint: cross-platform client, no drivers | Python + pyserial, standard CDC | ✅ | ✅ |
| Constraint: licenses | firmware/client GPL-3.0-or-later; TinyUSB MIT is compatible; code from free-pdk — with attribution | ✅ | ✅ |
| Constraint: submodules are reference only | code (the table, calibration stubs) is ported into `software/`; the build does not depend on the submodules | ✅ | ✅ |

No violations — Complexity Tracking is not filled in.

## Project Structure

### Documentation (this feature)

```text
specs/001-pc-programmer-protocol/
├── spec.md
├── plan.md                 # this file
├── research.md             # Phase 0
├── data-model.md           # Phase 1
├── quickstart.md           # Phase 1
├── contracts/
│   ├── protocol.md         # frames, commands, events, status codes
│   ├── package-format.md   # the firmware package format
│   └── chip-engine.md      # internal contracts: power, engine, local start
├── checklists/requirements.md
└── tasks.md                # Phase 2 (/speckit-tasks)
```

### Source Code (repository root)

```text
software/
├── firmware/
│   ├── CMakeLists.txt                # PLATFORM=stm32f072 | host
│   ├── cmake/arm-none-eabi.cmake
│   ├── core/                         # platform-independent C11
│   │   ├── proto/                    # cobs.c, crc32.c, frame.c, session.c, dispatch.c
│   │   ├── package/                  # header.c, validate.c, upload.c, pkg_cmds.c, effective_image.c, image_crc.c, serial_rng.c
│   │   ├── storage/                  # slots.c, journal.c, package_store.c, stats_cmds.c, config_cmds.c
│   │   ├── power/                    # power.c — the voltage settling policy (FR-040)
│   │   ├── ops/                      # op_manager.c, op_cmds.c, op_program.c, op_simple.c, op_patch.c, op_read.c, op_copy.c,
│   │   │                             #   op_restore.c, op_selftest.c, op_run.c, local_start.c, indication.c
│   │   └── chips/                    # chip_table.c, calib_sites.c, calib_footprint.c, serial_sites.c
│   ├── hal/                          # hal_flash.h, hal_crc.h, hal_transport.h, hal_power.h, hal_entropy.h, hal_ui.h, chip_engine.h
│   ├── platform/
│   │   ├── common/                   # shared by STM32: USB CDC (TinyUSB), the RUN USART bridge
│   │   ├── stm32g474/                # the programmer: WeAct STM32G474 (on-chip DAC/ADC, packages in bank 2 of the internal flash)
│   │   └── host/                     # a mock flash with a "power cut", a power model (with faults), an IC model, pdk-sim (stdio/TCP)
│   └── tests/                        # core unit tests (CTest)
├── client/
│   ├── pyproject.toml
│   ├── padauk_prog/
│   │   ├── transport.py              # serial / tcp://
│   │   ├── framing.py                # COBS, CRC, frames, stop-and-wait
│   │   ├── protocol.py               # commands, events, status codes
│   │   ├── package.py                # building a package (Header, mask, CRC)
│   │   ├── ihex.py                   # Intel HEX → words
│   │   └── cli.py                    # pdkprog: info/list/probe/read/write/erase/start/stats
│   └── tests/
│       ├── unit/
│       └── contract/                 # against pdk-sim, fault injection
└── tests/vectors/                    # shared test packages and frames for C and Python
docs/
└── protocol.md                       # the published version of contracts/ (FR-029)
```

**Structure Decision**: the firmware and the client are separate subprojects in `software/`; shared test vectors in
`software/tests/vectors/` guarantee that the C and Python implementations of frames and the package are bit-compatible.
The IC engine plugs in through `hal/chip_engine.h`: in this feature — a model for the simulator; the real PADAUK
algorithms (`core/engine`, used on the STM32G474) are feature `002-chip-engine` (R14). The F072 debug target and its
engine stub were removed later (constitution 1.5.0).

## Complexity Tracking

Not needed — there are no constitution violations.
