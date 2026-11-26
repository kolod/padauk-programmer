---

description: "Task list for 001-pc-programmer-protocol"
---

# Tasks: PC ⇄ programmer exchange protocol

**Input**: Design documents from `specs/001-pc-programmer-protocol/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md),
[contracts/protocol.md](contracts/protocol.md), [contracts/package-format.md](contracts/package-format.md),
[contracts/chip-engine.md](contracts/chip-engine.md), [quickstart.md](quickstart.md)

**Tests**: mandatory under constitution principle VI — core unit tests on the PC, client contract tests
against the `pdk-sim` simulator with fault injection, hardware validation tasks. In every story the tests are written
first and must fail before implementation.

**Organization**: tasks are grouped by user story (US1…US6 from spec.md).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on unfinished tasks)
- **[Story]**: the user story (US1…US6)

## Path Conventions

- Firmware: `software/firmware/` (`core/`, `hal/`, `platform/stm32f072/`, `platform/host/`, `tests/`)
- Client: `software/client/` (`padauk_prog/`, `tests/unit/`, `tests/contract/`)
- Shared test vectors: `software/tests/vectors/`
- All byte formats strictly follow `contracts/protocol.md`, `contracts/package-format.md`; internal
  interfaces follow `contracts/chip-engine.md`
- Every new file starts with `SPDX-License-Identifier: GPL-3.0-or-later`; code ported from
  `easy-pdk-programmer-software` keeps the free-pdk copyright. The submodules are not modified and are not a build dependency

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: the subproject skeleton, the build for two platforms, test frameworks

- [X] T001 Create the directory tree per plan.md: `software/firmware/{core/{proto,package,storage,power,ops,chips},hal,platform/{stm32f072,host},tests,cmake}`, `software/client/{padauk_prog,tests/{unit,contract}}`, `software/tests/vectors/` (with `.gitkeep` in empty ones)
- [X] T002 Create `software/firmware/CMakeLists.txt`: C11, `-Wall -Wextra -Werror`, a `PLATFORM` option (`host` | `stm32f072`), the `pdk_core` library from `core/*`, for `host` — the `pdk-sim` executable and CTest tests, for `stm32f072` — `pdkprog.elf/.bin`
- [X] T003 [P] Create the toolchain file `software/firmware/cmake/arm-none-eabi.cmake` (Cortex-M0, `-mcpu=cortex-m0 -mthumb -Os -flto -ffunction-sections -fdata-sections`, `--gc-sections`, `--specs=nano.specs`)
- [X] T004 [P] Create the minimal test framework `software/firmware/tests/pdk_test.h` (macros `TEST`, `ASSERT_EQ`, `ASSERT_MEM_EQ`, registration and running, a nonzero exit code on failure) and `software/firmware/tests/CMakeLists.txt`, which registers every `test_*.c` in CTest
- [X] T005 [P] Create `software/client/pyproject.toml`: package `padauk_prog`, Python ≥ 3.8, dependency `pyserial`, extra `dev` = `pytest`, console script `pdkprog = padauk_prog.cli:main`; `software/client/padauk_prog/__init__.py` with `__version__ = "0.1.0"`
- [X] T006 [P] Create `software/client/tests/conftest.py` with the option `--sim PATH` (the path to `pdk-sim`) and skipping the contract tests if the simulator is not given
- [X] T007 [P] Add to the root `.gitignore`: `build/`, `software/client/**/__pycache__/`, `*.egg-info/`, `.pytest_cache/`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: framing, CRC, the session, the HAL, the voltage policy, the IC table, the simulator, and the base client — needed by all stories

**⚠️ CRITICAL**: no story starts until this phase is complete

### Foundation tests (write first)

- [X] T008 [P] Generate the shared vectors `software/tests/vectors/frames.json` with the script `software/tests/vectors/gen_frames.py` on `zlib`: CRC-32 (`"123456789"` → `0xCBF43926`, an empty string, 1 KB of pseudo-random bytes), COBS (empty, `00`, `11 00 22`, 254 and 255 nonzero bytes, only zeros), 6 frames (REQ/RSP/EVT/NAK) in hex before and after COBS; the option `--c-header PATH` generates `software/firmware/tests/vectors_frames.h`
- [X] T009 [P] Unit tests of CRC-32 and COBS against the vectors in `software/firmware/tests/test_crc_cobs.c`
- [X] T010 [P] Frame unit tests in `software/firmware/tests/test_frame.c`: encoding/decoding of all types, rejection of a wrong length, CRC, `len > max_payload`, truncated COBS, resynchronization on the next `0x00` after garbage
- [X] T011 [P] Session unit tests in `software/firmware/tests/test_session.c`: a duplicated `seq` → the same response without re-executing (handler call count = 1), `NAK` on a corrupted frame, commands before `HELLO` → `NO_SESSION`, `HELLO` with a different major version → `PROTO_VERSION` + `INFO`, resetting the session and an unfinished upload on a DTR change (an IC operation is not cancelled — FR-023), an unknown `code` → `UNKNOWN_COMMAND`
- [X] T012 [P] Unit tests of the voltage settling policy in `software/firmware/tests/test_power.c` with a fake `hal_power` (contracts/chip-engine.md §2, protocol §6.1): `power_off_all()` — the first step of `pdk_core_init()` before any other initialization (removing voltages after reset, principle III); the stage's minimum pause is observed even with instant settling; 3 consecutive measurements within ±2 % / ±30 mV (whichever is larger); a 20 ms timeout → `VDD_FAULT`/`VPP_FAULT` with `rail/set_mV/meas_mV` and a call to `power_off_all()`; 0 V on removal falls under the same rule
- [X] T013 [P] Client framing unit tests in `software/client/tests/unit/test_framing.py` against `software/tests/vectors/frames.json`
- [X] T014 [P] Client stop-and-wait unit tests in `software/client/tests/unit/test_reliability.py` with a fake transport: a retry with the same `seq` after `NAK`, after a corrupted response, after a 300 ms timeout; a session error after 5 retries; events between responses are delivered separately

### Foundation implementation — firmware

- [X] T015 [P] Declare the status, command, event, operation-type, stage, `warnings`, `site_status`, and `STATUS.flags/selftest_state` codes from `contracts/protocol.md` §4–§6 in `software/firmware/core/proto/proto_defs.h` (enums with exact numeric values; mark reserved codes with a comment) and `PDK_PROTO_MAJOR 1`, `PDK_PROTO_MINOR 0`
- [X] T016 [P] Implement CRC-32/ISO-HDLC (a nibble table 16×u32, an incremental API `crc32_init/update/final`) in `software/firmware/core/proto/crc32.c` and `crc32.h`
- [X] T017 [P] Implement the COBS encoder and a streaming decoder with resynchronization in `software/firmware/core/proto/cobs.c` and `cobs.h`
- [X] T018 Implement frame encoding/parsing (`frame_encode`, `frame_parse` → `FRAME_ERROR`) in `software/firmware/core/proto/frame.c` and `frame.h` (depends on T016, T017)
- [X] T019 [P] Declare the HAL interfaces in `software/firmware/hal/` per `contracts/chip-engine.md`: `hal_transport.h` (read/write, `is_open()` = DTR, a state-change event), `hal_flash.h` (slot and log geometry, erase_page, program, read), `hal_crc.h` (a hardware CRC-32 with a fallback to `core/proto/crc32.c`), `hal_power.h` (`power_set`, `power_measure`, `power_off_all`, `power_limits`), `hal_entropy.h` (raw noise bits, the MCU UID), `hal_ui.h` (`local_start(request)` as an event from the platform, `ui_state(state)`), `hal_time.h` (milliseconds, microseconds), `chip_engine.h` (the primitives and the 5 guarantees from §3, the `poll` callback)
- [X] T020 Implement the voltage settling policy `power_apply(rail, mV, min_pause_us)` per contracts/chip-engine.md §2 in `software/firmware/core/power/power.c` and `power.h` (depends on T019)
- [X] T021 Implement the session (port state, `HELLO`, the cache of the last response, the event counter, `NAK`, the payload limit) in `software/firmware/core/proto/session.c` and `session.h` (depends on T018, T019)
- [X] T022 Implement the command dispatcher (a `code → handler` table, an "allowed during an operation" flag, `BUSY`/`UNKNOWN_COMMAND`/`BAD_PAYLOAD`) and the handlers `HELLO`, `GET_INFO` (the `INFO` structure from contracts §4, the capability bits) in `software/firmware/core/proto/dispatch.c` and `dispatch.h` (depends on T021)
- [X] T023 Port the IC table from `easy-pdk-programmer-software/fpdkicdata.c` (13 entries, with the free-pdk copyright) into `software/firmware/core/chips/chip_table.c` and `chip_table.h`: a public part (`CHIP_DESC`) and an internal one (voltages, stage minimum pauses, write block parameters, erase pulses, the IO addresses of `IHRCR`/`ILRCR`/`BGTR`/`GPCC`/`GPCS` from `easy-pdk-programmer-software/Examples/src/easypdk/*.h`; unknown ones — `0xFF`); lookup by ID and by name/variant
- [X] T024 Implement the `LIST_CHIPS` command with pagination by `first_index` in `software/firmware/core/chips/chip_list_cmd.c` and register it in the dispatcher (depends on T022, T023)
- [X] T025 Implement the core main loop `pdk_core_init()` (the first step — `power_off_all()` and the IC lines Hi-Z) / `pdk_core_poll()` (transport → decoder → session → dispatcher → send) in `software/firmware/core/pdk_core.c` and `pdk_core.h` (depends on T022)
- [X] T026 [P] Implement the host HAL: stdio and TCP transport (`--tcp host:port`, DTR = the TCP connection), time, a software CRC, OS entropy in `software/firmware/platform/host/hal_host_transport.c`, `hal_host_misc.c`
- [X] T027 [P] Implement the flash emulation for the host with the F072 geometry (2 KB pages, slots `0x0800B000`/`0x0800D000`, the log `0x0800F000`), a state file (`--flash FILE`), the "program only erased" rule, injection of a power cut after the N-th write/erase operation (`--power-fail-after N`, exit code 99), and read corruption during a write (`--corrupt-flash-during-write`) in `software/firmware/platform/host/hal_host_flash.c`
- [X] T028 [P] Implement the power model for the host (VDD/VPP sources with a settling time constant, measurement noise, the faults `--power-fault vdd:low|vpp:slow|vpp:stuck`, a time-under-voltage counter for checking SC-011) in `software/firmware/platform/host/hal_host_power.c`
- [X] T029 Implement `pdk-sim` (`main`: the arguments `--tcp`, `--flash`, `--chip`, `--chip-state`, `--power-fail-after`, `--power-fault`, `--corrupt-flash-during-write`, the stdin commands `local-start program|copy|verify|selftest|restore`, `local-cancel`, `swap-chip SPEC`; the `pdk_core_poll` loop; `ui_state` output to stderr) in `software/firmware/platform/host/sim_main.c` (depends on T025, T026, T027, T028)
- [X] T030 [P] Implement the STM32F072 debug target platform (Easy PDK boards; the primary MCU is the next specification): `startup.s`, a linker script with the flash layout from research R5 (`FLASH 44K`, the symbols `__slot0`, `__slot1`, `__journal`), 48 MHz clocking from HSE + HSI48 for USB in `software/firmware/platform/stm32f072/` (`startup.s`, `stm32f072c8.ld`, `system.c`)
- [X] T031 Integrate TinyUSB (FetchContent with a pinned tag in `software/firmware/CMakeLists.txt`; CDC ACM, VID/PID `0x1209:0x0001`, descriptors), the HAL transport with DTR via `tud_cdc_connected()` in `software/firmware/platform/stm32f072/usb_descriptors.c`, `tusb_config.h`, `hal_f072_transport.c` (depends on T030)
- [X] T032 [P] Implement the HAL for F072: flash (page erase, half-word programming), a hardware CRC (`REV_IN`/`REV_OUT`), SysTick and a microsecond timer, entropy (the low bits of the ADC on VREFINT/the temperature sensor + the 96-bit UID), power (DAC + ADC for VDD/VPP per the Easy PDK schematic, active discharge — through DAC = 0), a `hal_ui` stub (button PA15 → `local_start(PROGRAM)`, LEDs PC13–PC15) in `software/firmware/platform/stm32f072/hal_f072_flash.c`, `hal_f072_crc.c`, `hal_f072_power.c`, `hal_f072_misc.c`
- [X] T033 Implement `main()` for F072 (at the very start — the VDD/VPP DAC = 0 and the IC lines Hi-Z before clocking and USB; then initialization, the TinyUSB task + `pdk_core_poll`) and a `chip_engine` stub (all primitives → `OP_UNSUPPORTED`, `safe_state` = `power_off_all()` + the IC lines Hi-Z) in `software/firmware/platform/stm32f072/main.c`, `chip_engine_stub.c` (depends on T031, T032)

### Foundation implementation — client

- [X] T034 [P] Implement CRC/COBS/frames in `software/client/padauk_prog/framing.py` (`zlib.crc32`, a streaming COBS decoder)
- [X] T035 [P] Implement the `serial` (pyserial, DTR=1) and `tcp://host:port` transports with automatic port discovery by VID/PID `0x1209:0x0001` in `software/client/padauk_prog/transport.py`
- [X] T036 Implement `software/client/padauk_prog/protocol.py`: code enums from contracts §4–§6, a `Programmer` class (stop-and-wait, retries, an event queue, `hello()`, `get_info()`, `list_chips()` with parsing of `INFO` and `CHIP_DESC`), a `ProgrammerError(status, payload)` exception (depends on T034, T035)
- [X] T037 Implement the TCP fault-injection proxy `software/client/tests/contract/fault_proxy.py` (bit corruption, truncation, duplication, frame delay in both directions, a deterministic seed, a forced connection drop) and the fixtures `pdk_sim`, `fault_proxy`, a latency measurement of every RSP in `software/client/tests/contract/conftest.py` (depends on T029, T036)

**Checkpoint**: `ctest` and `pytest tests/unit` are green; the client library gets `INFO` and `LIST_CHIPS` from `pdk-sim` and from F072

---

## Phase 3: User Story 1 — Uploading a firmware package to the programmer (Priority: P1) 🎯 MVP

**Goal**: the client builds a package from HEX and sends it with an integrity guarantee; the programmer checks it against its own table and atomically stores it, returning the CRC

**Independent Test**: upload a package, "restart" `pdk-sim` (the same `--flash`), `PKG_INFO` → the CRC, name, and header match the client's

### Tests for User Story 1

- [X] T038 [P] [US1] Generate the package vectors `software/tests/vectors/packages/` with the script `software/tests/vectors/gen_packages.py`: a valid PFS154 of 3 words (the reference from package-format), a valid `helloworld_pfs154` with an IHRC stub, a valid package with a serial-number stub, a package without calibration stubs; one invalid one for each row of the `package-format.md` check tables (including: an empty/invalid `name`, nonzero reserved bits and fields, `CREATED_BY_COPY` from the client, an OTP package PMS150C with `SKIP_BLANK_CHECK`, an unknown stub of 6+ `AND A`) with the expected code and `addr` in `manifest.json`
- [X] T039 [P] [US1] Unit tests of header and commit validation in `software/firmware/tests/test_package_validate.c` per `manifest.json` (each invalid vector → the exact code and `addr`; `calib_sites`/`serial_sites` for the valid ones)
- [X] T040 [P] [US1] Unit tests of the search for calibration stubs and serial-number stubs in `software/firmware/tests/test_sites.c`: templates for 13/14/15-bit cores, a position anywhere in memory, up to 16 stubs, ≥ 6 unknown `AND A` → `PKG_CALIB_UNKNOWN` with an address, no false matches on the examples from `easy-pdk-programmer-software/Examples/*.ihx`
- [X] T041 [P] [US1] State log unit tests in `software/firmware/tests/test_journal.c`: a 40 B record and the CRC of bytes 0–35, choosing the current record by `rec_seq`, ignoring an incompletely written record, page overflow and the switch to the second one, a power cut during the erase of the second page
- [X] T042 [P] [US1] Upload unit tests in `software/firmware/tests/test_upload.c`: sequential `PKG_DATA`, `PKG_OFFSET` with `expected`, a mismatch of the `PKG_DATA` and `PKG_BEGIN` header, `PKG_ABORT`, a dropped session, gradual page erase (≤ 1 page per call), an atomic commit
- [X] T043 [P] [US1] Client unit tests in `software/client/tests/unit/test_ihex_package.py`: Intel HEX parsing (byte addresses → LE words, a mask of defined words), the default name from the file name, building a package bit-identical to the T038 vectors
- [X] T044 [P] [US1] Contract test `software/client/tests/contract/test_upload.py`: upload → `PKG_INFO`/`PKG_READ` match byte for byte; restarting the simulator keeps the package; each invalid vector → the exact code; the active package does not change after a rejection; a package without stubs → `calib_sites = 0` and a client warning (FR-050)
- [X] T045 [P] [US1] Contract test SC-001 `software/client/tests/contract/test_frame_corruption.py`: 1000 uploads of a `max_package_size` package through `fault_proxy` with ≥ 1 % corrupted/lost/duplicated frames in each direction; every upload → the stored package is identical to the source
- [X] T046 [P] [US1] Contract test SC-007 `software/client/tests/contract/test_power_loss.py`: ≥ 100 runs of `pdk-sim --power-fail-after N` with a random N in the middle of an upload; after a restart `pkg_state = VALID` and the CRC = the old or the new package
- [X] T047 [P] [US1] Contract test `software/client/tests/contract/test_duplicate_seq.py`: a repeated `PKG_DATA` with the same `seq` does not duplicate data; a repeated `PKG_COMMIT` does not make a second log record
- [X] T048 [P] [US1] Contract test FR-024 `software/client/tests/contract/test_latency.py`: every RSP ≤ 100 ms, separately — `PKG_DATA` at a flash page boundary, `PKG_COMMIT` of a `max_package_size` package, a log write that moves to another page

### Implementation for User Story 1

- [X] T049 [P] [US1] Implement the header structure and parsing (the fields from package-format, `name`/`label` UTF-8, reserved bits and fields = 0), the `PKG_BEGIN`-level checks (including `SKIP_BLANK_CHECK` for OTP → `PKG_OTP_BLANK_CHECK_REQUIRED`, `CREATED_BY_COPY` from the client → `PKG_HEADER_INVALID`) in `software/firmware/core/package/header.c` and `header.h`
- [X] T050 [P] [US1] Port the calibration stub templates (IHRC/ILRC/BG for 13/14/15 bit, the measurement loops and post-calibration patches from `easy-pdk-programmer-software/fpdkiccalib.c`, with the copyright) and the stub search per research R8 into `software/firmware/core/chips/calib_sites.c` and `calib_sites.h`
- [X] T051 [P] [US1] Port the serial-number stub templates (`RET 'F','P','S','E','R','I','A','L'` for 13/14/15/16 bit, from `fpdkicserial.c`) and the position search into `software/firmware/core/chips/serial_sites.c` and `serial_sites.h`
- [X] T052 [US1] Implement the full package check at commit (the CRC from flash through `hal_crc`, reserved areas, the fuse from the last word vs `FUSE_PRESENT`, the protection bit vs `ALLOW_SECURITY_FUSE`, stub recognizability; the result `calib_sites`, `serial_sites`, `effective_words`, a code + `addr`) in `software/firmware/core/package/validate.c` and `validate.h` (depends on T049, T050, T051)
- [X] T053 [P] [US1] Implement the slots (addresses from `hal_flash`, choosing the inactive one, gradual page erase during a write, reading) in `software/firmware/core/storage/slots.c` and `slots.h`
- [X] T054 [P] [US1] Implement the state log (the 40 B `StateRecord` from the data-model, finding the current record, appending, page overflow, copying to the second page) in `software/firmware/core/storage/journal.c` and `journal.h`
- [X] T055 [US1] Implement `PackageState` (EMPTY/VALID/CORRUPTED) at start and on request in `software/firmware/core/storage/package_store.c` and `package_store.h` (depends on T053, T054)
- [X] T056 [US1] Implement the upload state machine (IDLE/RECEIVING/COMPLETE) and the commands `PKG_BEGIN`, `PKG_DATA`, `PKG_COMMIT`, `PKG_ABORT`, resetting the upload on a dropped session, the `EVT_PKG_STATE` event in `software/firmware/core/package/upload.c` and `upload.h` (depends on T052, T055)
- [X] T057 [US1] Implement the commands `PKG_INFO`, `PKG_READ`, `PKG_DELETE`, `PKG_CHECK` and register all `PKG_*` in the dispatcher in `software/firmware/core/package/pkg_cmds.c` (depends on T056)
- [X] T058 [P] [US1] Implement Intel HEX parsing (records 00/01/02/04, line checksums) and 8-bit Intel HEX writing in `software/client/padauk_prog/ihex.py`
- [X] T059 [US1] Implement package building (a 128 B Header with `header_crc32`, a default `name` from the file name, `label`, words, the mask, `package_crc32`, flags from options, a size check against `CHIP_DESC` before sending) in `software/client/padauk_prog/package.py` (depends on T058)
- [X] T060 [US1] Add to `Programmer` the methods `upload_package()` (BEGIN, DATA in chunks ≤ `max_payload − 4`, COMMIT, CRC comparison), `package_info()`, `package_read()`, `package_delete()`, `package_check()` in `software/client/padauk_prog/protocol.py` (depends on T059)
- [X] T061 [US1] Implement the CLI `software/client/padauk_prog/cli.py`: global `--port`, `-v`; `upload FILE -n NAME [--fuse --allowsecfuse --securefill --noerase --noblankchk --noverify --runvdd --name --label]`; a refusal on `--nocalibrate` ("remove the calibration macro") and `--serial` ("the number is random, automatic"); a refusal on `--noblankchk` for OTP by `CHIP_DESC.family`; `pkg info|read FILE|delete|check`; a message for each `PKG_*` with `addr`; a warning when `calib_sites = 0` (depends on T060)

**Checkpoint**: US1 works on its own on `pdk-sim`; T044–T048 are green

---

## Phase 4: User Story 2 — Programming an IC from the PC with progress and a result (Priority: P1)

**Goal**: `PROGRAM` — the programmer performs all steps (including an OTP retry, calibration, the final check), sends progress and a summary; `VERIFY` taking stubs into account; `PATCH`; cancellation; a USB drop does not interrupt programming

**Independent Test**: on `pdk-sim --chip PFS154` a client without an IC table programs a package and sees all stages, progress ≥ 1/s, a `RESULT` with calibration, a random serial number, and content sums

### Tests for User Story 2

- [X] T062 [P] [US2] Effective image unit tests in `software/firmware/tests/test_effective_image.c`: undefined words, `SECURE_FILL` up to reserved areas, fuse exclusion, a serial-number stub (least significant byte first), a calibration stub → a measurement loop → the finished form (compared with the opcodes of `fpdkiccalib.c`); all "loop → finished" transitions are only 1→0
- [X] T063 [P] [US2] Generate the calibrated dump vectors `software/tests/vectors/footprints/` with the script `software/tests/vectors/gen_footprints.py`: for 13/14/15 bit — finished IHRC/ILRC/BG traces, measurement loops with an arbitrary `MOV A,#…`, traces with a "foreign" register address, a constant write to `IHRCR` outside a trace; the expected restored stubs and normalized sums in `manifest.json`
- [X] T064 [P] [US2] Unit tests of the trace and loop detector in `software/firmware/tests/test_calib_footprint.c` against the T063 vectors: position, type, `k`; recognizing a measurement loop; restoring a stub bit for bit as in free-pdk; a non-matching register address → not a trace
- [X] T065 [P] [US2] Content sum unit tests in `software/firmware/tests/test_image_crc.c`: skipping reserved areas and `reserved_first_word`, zeroing bits above `code_bits`, normalizing traces and serial-number stubs; two ICs with the same firmware, different `k` and serial number → the same `image_crc_norm`, a different `image_crc_raw`; the sum from the package's effective image = the sum of the model's contents after `PROGRAM`
- [X] T066 [P] [US2] Serial-number generator unit tests in `software/firmware/tests/test_serial_rng.c` with a fake `hal_entropy`: 64 bits, different values for consecutive ICs and for different UIDs with the same "entropy", a repetition/proportion test of raw bits → a source failure = the operation does not start (`INTERNAL`)
- [X] T067 [P] [US2] Operation manager unit tests in `software/firmware/tests/test_op_manager.c` with a fake engine: the `PROGRAM` stage sequence (`PKG_CHECK → ID → ERASE → BLANK_CHECK → WRITE → VERIFY → CALIBRATE → PKG_CHECK → FUSE → FINAL_VERIFY`), skipping by flags, `EVT_PROGRESS` on a stage change and ≤ 500 ms, `OP_CANCEL` → `CANCELLED` + `safe_state`, a dropped session does **not** cancel the operation, `BUSY` for forbidden commands and handling of those allowed during an operation, a `StateRecord` write after completion, every voltage change through `power_apply`
- [X] T068 [P] [US2] `PROGRAM` branch unit tests in `software/firmware/tests/test_program_branches.c`: OTP blank / compatible (a retry, `RESUMED`) / everything already written (`CHIP_ALREADY_PROGRAMMED`) / incompatible (`CHIP_NOT_BLANK` with no write); a serial-number stub blank → a new number, written → the number from the IC; `CALIB_FAILED` → nothing written after the loop, the fuse not written; `CALIB_FINALIZE_ON_FAIL` only with `source = 0`; a streaming CRC mismatch → `STORE_CORRUPTED` before the fuse (FR-049); `FINAL_VERIFY` catches a "weak" word (FR-048)
- [X] T069 [P] [US2] `VERIFY` and `PATCH` unit tests in `software/firmware/tests/test_verify_patch.c`: `VERIFY` taking stubs into account (a finished trace with any `k` → OK + the actual `k` and serial number; a loop → `VERIFY_FAILED`, `site_status = FAILED`); `PATCH`: the rule `(current AND value) == value`, a conflict → `PATCH_CONFLICT` with no write, `expect_mode` RAW/NORMALIZED/ACTIVE_PACKAGE → `PATCH_IMAGE_MISMATCH` with no write, a reserved area, `addr ≥ code_words`, `count` 0/>64, a protection fuse, `CHECK_ONLY`, a full check after writing
- [X] T070 [P] [US2] Contract test `software/client/tests/contract/test_program.py`: `PROGRAM` on a PFS154 model → all stages in order, a `RESULT` with `calib_count = 1` within ±10 %, a random `serial_written`, `image_crc_raw/norm`; the model's contents = the effective image with the actual `k`; `CHIP_ID_MISMATCH` with `chip_id_found`; `CHIP_NOT_BLANK` for an incompatible OTP model
- [X] T071 [P] [US2] Contract test `software/client/tests/contract/test_busy_cancel.py`: during `PROGRAM` `PKG_BEGIN`/`OP_START` → `BUSY`, `GET_STATUS`/`OP_RESULT` work; `OP_CANCEL` → `CANCELLED` with the stage, voltages removed
- [X] T072 [P] [US2] Contract test SC-008 `software/client/tests/contract/test_disconnect.py`: ≥ 100 TCP drops at an arbitrary moment of `PROGRAM` → the model's contents as without the drop, after reconnecting `OP_RESULT(0)` → `OK`
- [X] T073 [P] [US2] Contract test SC-002 `software/client/tests/contract/test_stored_corruption.py`: corrupts each byte of the package in the simulator's flash file in turn; `PROGRAM` → `STORE_CORRUPTED` at `PKG_CHECK`, the IC model unchanged
- [X] T074 [P] [US2] Contract test SC-012 `software/client/tests/contract/test_otp_retry.py`: ≥ 100 "power cuts" at an arbitrary point of `PROGRAM` of the OTP model PMS150C (`--chip-state`), a repeated `PROGRAM` with the same and with a "different" simulator (a separate `--flash`, the same package) → `OK` + `RESUMED` or `CHIP_NOT_BLANK` with the model unchanged; no extra zeros; a written serial number is kept
- [X] T075 [P] [US2] Contract test SC-013 `software/client/tests/contract/test_calib_fail.py`: a model with a "faulty" oscillator → `CALIB_FAILED`, nothing written after the loop; with `CALIB_FINALIZE_ON_FAIL` → `CALIB_FORCED`; after a "repair" a repeated `PROGRAM` → `OK` + `RESUMED`
- [X] T076 [P] [US2] Contract test SC-011 `software/client/tests/contract/test_power_fault.py`: `--power-fault vpp:slow|vpp:stuck|vdd:low` → `VPP_FAULT`/`VDD_FAULT` ≤ 20 ms after the change, `RESULT.v_rail/v_set_mV/v_meas_mV` filled in, the IC model with no write at all
- [X] T077 [P] [US2] Contract test `software/client/tests/contract/test_verify_final.py`: `VERIFY` of a calibrated IC with a serial number → `OK` with the actual `k` and number; unfinished calibration → `VERIFY_FAILED`; a model with a "weak" write of a stub word or the fuse → `VERIFY_FAILED` at `FINAL_VERIFY`; `--corrupt-flash-during-write` → `STORE_CORRUPTED` before the fuse
- [X] T078 [P] [US2] Contract test `software/client/tests/contract/test_patch.py` on a programmed OTP model PMS150C: a compatible patch with `--expect-package` → only the given words are changed and verified; a batch of three models with different IHRC offsets and serial numbers passes with one normalized sum; an IC with different firmware → `PATCH_IMAGE_MISMATCH`, the model unchanged; a 0→1 conflict → `PATCH_CONFLICT`, the model unchanged; `--check` shows both sums and writes nothing

- [X] T131 [P] [US2] Unit tests of the active-stub check (FR-055) in `software/firmware/tests/test_calib_site_check.c` with a fake engine: two stubs IHRC + ILRC, the IC "stuck" in the one with the higher address → `CALIB_WRONG_SITE` at the first measurement (a frequency ×100 off the expected one), nothing written; a model with no response to "+1" or with a chaotic frequency → `CALIB_WRONG_SITE`; BG with a "floating" PA0 → `CALIB_WRONG_SITE`; `CALIB_FINALIZE_ON_FAIL` does not apply; the correct order — `OK`
- [X] T132 [P] [US2] Contract test `software/client/tests/contract/test_calib_wrong_site.py`: an IC model with the mode "executes stubs out of address order" (`--chip PFS154:order=reverse`) → `PROGRAM` ends with `CALIB_WRONG_SITE`, the model's contents after the measurement loops are unchanged

### Implementation for User Story 2

- [X] T079 [US2] Implement the IC model for the simulator (depends on T050 — the measurement loop opcodes) (an in-memory `chip_engine`: the ID from `--chip`, FLASH/OTP 1→0 semantics, erase, an erase counter; an IHRC/ILRC model: frequency = f(register) × (1 + the offset `offset=±N%`), recognizing a written measurement loop and "generating" a frequency on PA3 taking the PA4 pulses into account; a BG comparator model; the modes `osc=broken`, `weak=ADDR`; saving state `--chip-state`; `safe_state`) in `software/firmware/platform/host/chip_model.c`
- [X] T080 [US2] Implement the detector of calibration traces and measurement loops and the restoring of stubs in `software/firmware/core/chips/calib_footprint.c` and `calib_footprint.h` (depends on T050, T023)
- [X] T081 [US2] Implement the effective image (`EffectiveImagePlan`: the word by address, stub states, substituting the actual `k` and serial number, the fuse) in `software/firmware/core/package/effective_image.c` and `effective_image.h` (depends on T050, T051, T052)
- [X] T082 [US2] Implement the content sums `image_crc_raw/norm` (a streaming normalizer with the shared detector from T080 and serial-number stubs — 8 `RET #imm`; computing from the package's effective image) in `software/firmware/core/package/image_crc.c` and `image_crc.h` (depends on T080, T081)
- [X] T083 [P] [US2] Implement the serial-number generator (≥ 256 raw bits from `hal_entropy` + UID + `rec_seq` + a RAM counter → BLAKE2s, 64 bits; simplified repetition/proportion tests) in `software/firmware/core/package/serial_rng.c` and `serial_rng.h`
- [X] T084 [US2] Implement the operation manager (IDLE/RUNNING/DONE, `op_id`, stages and percents, `poll` for the engine with handling of the allowed commands, cancellation, a dropped session does not interrupt the operation, `RESULT`) in `software/firmware/core/ops/op_manager.c` and `op_manager.h` (depends on T025, T020)
- [X] T085 [US2] Implement the `PROGRAM` pipeline per the data-model `ProgramJob` (PKG_CHECK; ID; ERASE for FLASH; BLANK_CHECK with an OTP compatibility check and a retry; WRITE with a streaming CRC; VERIFY; CALIBRATE of each stub with the FR-043 policy and `CALIB_FINALIZE_ON_FAIL`; PKG_CHECK; FUSE; FINAL_VERIFY with content sums; a random serial number; `op_flags.SKIP_BLANK_CHECK` for OTP → `PKG_OTP_BLANK_CHECK_REQUIRED`) in `software/firmware/core/ops/op_program.c` (depends on T081, T082, T083, T084, T055)
- [X] T086 [US2] Implement the `VERIFY` (taking stubs into account, FR-047), `PROBE`, `ERASE`, `BLANK_CHECK` pipelines in `software/firmware/core/ops/op_simple.c` (depends on T081, T082, T084)
- [X] T087 [US2] Implement the `PATCH` operation (parsing `ext`, `ID` → reading all memory → checking the sum by `expect_mode` and the compatibility of all words before any write → writing only the changed words in blocks with ones → a full check; `CHECK_ONLY`; the prohibitions on reserved areas and the protection fuse) in `software/firmware/core/ops/op_patch.c` (depends on T082, T084)
- [X] T088 [US2] Implement the commands `OP_START`, `OP_CANCEL`, `OP_RESULT`, `GET_STATUS` (the `STATUS` and `RESULT` structures with all the fields of contracts §4) and the events `EVT_OP_STARTED`, `EVT_PROGRESS`, `EVT_OP_DONE` in `software/firmware/core/ops/op_cmds.c` (depends on T085, T086, T087)
- [X] T089 [US2] Connect the IC model in `pdk-sim` (`--chip NAME[:offset=..,osc=..,weak=..]|none|blank|written`) in `software/firmware/platform/host/sim_main.c` (depends on T079, T088)
- [X] T090 [US2] Add to `Programmer` the methods `start_op()`, `cancel_op()`, `op_result()`, `get_status()`, `patch()`, an event iterator that parses `EVT_PROGRESS`/`RESULT` in `software/client/padauk_prog/protocol.py` (depends on T060)
- [X] T091 [US2] Add the CLI subcommands `write FILE -n NAME [upload options]` (= upload + `PROGRAM`), `program`, `verify`, `probe`, `erase -n NAME`, `blankcheck -n NAME`, `status`; a progress line; a summary with calibration, serial number, sums, the mismatch address, `RESUMED`/`CALIB_FORCED`; Ctrl+C → `OP_CANCEL`; `--finalize-on-calib-fail` in `software/client/padauk_prog/cli.py` (depends on T090)
- [X] T133 [US2] Implement the FR-055 checks in the `calibrate` primitive (plausibility of the first measurement 1/4…4 of `target_hz / 7`, monotonicity of the response to a step, PA0 driven for BG) in the IC engine and the IC model (`order=reverse`, "no response") in `software/firmware/platform/host/chip_model.c`; the client shows `CALIB_WRONG_SITE` with the explanation "stubs are executed out of address order" (depends on T079, T085)
- [X] T092 [US2] Add the CLI subcommand `patch -n NAME ADDR=VALUE … (--expect-crc CRC [--normalized] | --expect-package) [--check] [--allowsecfuse]` (without an expected sum — only `--check`; a conflict in binary form) in `software/client/padauk_prog/cli.py` (depends on T090)

**Checkpoint**: US1 + US2 work on `pdk-sim`; T062–T078 are green

---

## Phase 5: User Story 4 — Information about the programmer, ICs, and the self-test (Priority: P2)

**Goal**: the client shows versions, capabilities, and the IC table from the programmer; an incompatible version blocks operations; `SELF_TEST` without an IC (automatically at start and from a PC)

**Independent Test**: the unchanged client sees an extra IC from the `pdk-sim-extra` build; `pdkprog selftest` with an empty "socket" shows all VDD/VPP levels, and with an IC — a refusal without high voltages

### Tests for User Story 4

- [X] T093 [P] [US4] Self-test unit tests in `software/firmware/tests/test_selftest.c` with a fake `hal_power` and engine: the levels = all distinct VDD/VPP values from the IC table in ascending order + 0 V after each rail; the other rail = 0; probe first, an IC response → `SELFTEST_CHIP_PRESENT` with no levels above probe; the first level not reached → `SELFTEST_FAILED`, `safe_state`; `RESULT.levels[]`; autostart at power-up with `source = 1`; `STATUS.selftest_state`
- [X] T094 [P] [US4] Contract test `software/client/tests/contract/test_info.py`: `INFO` (version 1.0, `max_payload`, `max_package_size`, `max_read_words`, the capability bits), `LIST_CHIPS` with pagination, major version 2 → `PROTO_VERSION`, `OP_START`/`PKG_BEGIN` → `NO_SESSION`; SC-006 with `pdk-sim-extra`
- [X] T095 [P] [US4] Contract test `software/client/tests/contract/test_selftest.py`: `--chip none` → `OK` with all levels; `--chip PFS154` → `SELFTEST_CHIP_PRESENT`; `--power-fault vpp:stuck` → `SELFTEST_FAILED` with a level; after the simulator starts `STATUS.selftest_state` = 2/3/4 respectively

### Implementation for User Story 4

- [X] T096 [US4] Implement the `SELF_TEST` operation (contracts §4 `SELF_TEST`, research R15) and the autostart at core start in `software/firmware/core/ops/op_selftest.c` and `software/firmware/core/pdk_core.c` (depends on T020, T084, T023)
- [X] T097 [US4] Add a test IC entry under `#ifdef PDK_TEST_EXTRA_CHIP` in `software/firmware/core/chips/chip_table.c` and the target `pdk-sim-extra` in `software/firmware/CMakeLists.txt`
- [X] T098 [US4] Add the CLI subcommands `info` (versions, platform, capabilities, package state, `selftest_state`), `list` (like `easypdkprog list`: name, ID, FLASH/OTP, words, word width, RAM, `(RO)`), `selftest` (the "socket is empty" confirmation, a table of levels set/measured/time) in `software/client/padauk_prog/cli.py` (depends on T090)

**Checkpoint**: US4 is verified; the client has no IC table (`grep` for IC names in `software/client/padauk_prog/` is empty)

---

## Phase 6: User Story 3 — Standalone programming with an integrity check (Priority: P2)

**Goal**: a local start without a PC; a corrupted package is not written; local programming is blocked until a successful self-test; each IC gets a random serial number; statistics survive power-off

**Independent Test**: `pdk-sim` with a corrupted flash → `local-start program` does not write, an error indication; with an intact package — three local starts give three different serial numbers; with `--power-fault` after start a local start runs `SELF_TEST`, not `PROGRAM`

### Tests for User Story 3

- [X] T099 [P] [US3] Local start unit tests in `software/firmware/tests/test_local_start.c`: the full FR-054 action set (`PROGRAM`, `COPY`, `VERIFY`, `SELF_TEST`, `RESTORE_MASTER`, `local_cancel`); priority (the self-test did not pass → `SELF_TEST`; `MASTER_RESTORE_PENDING` → `RESTORE_MASTER`); `RESTORE_MASTER` without `PENDING` is ignored; the `ui_state` sequence for each result with the right cause class; a request during an operation is ignored; `EMPTY`/`CORRUPTED` → no voltages, a `ui_state` integrity error; a local operation is not cancelled by a dropped session; `CALIB_FINALIZE_ON_FAIL` is ignored
- [X] T100 [P] [US3] Contract test `software/client/tests/contract/test_autonomous.py`: three `local-start program` → three different `serial_written`; restarting the simulator keeps `ok/fail`; with a connected client `EVT_OP_STARTED(source=1)` and `EVT_OP_DONE` arrive; a corrupted package → no write to the model; a faulty supply at start → `LOCAL_START_BLOCKED`, `local-start` → `SELF_TEST`; `local-start verify` and `local-start selftest` give the corresponding `RESULT`; `local-cancel` in the middle of programming → `CANCELLED`, voltages removed; the simulator's `ui_state` output matches FR-054

### Implementation for User Story 3

- [X] T101 [US3] Implement the FR-054 local actions (contracts/chip-engine.md §4: `local_start(request)` for all requests, `local_cancel()`) with conversion by the research R16 priority, a `PackageState` check before programming, and `STATUS.flags.LOCAL_START_BLOCKED` in `software/firmware/core/ops/local_start.c` and `local_start.h` (depends on T085, T096)
- [X] T102 [P] [US3] Implement the indication states `ui_state(state, detail)` per contracts/chip-engine.md §4 (`READY`, `BUSY` with a stage and percent, `SUCCESS`, `FAIL` with a cause class and code, `BLOCKED`) in `software/firmware/core/ops/indication.c` and `indication.h` and the mapping to 3 LEDs in `software/firmware/platform/stm32f072/hal_f072_misc.c`
- [X] T103 [US3] Implement `STATS_GET` and `STATS_RESET` (ok/fail in the log) in `software/firmware/core/storage/stats_cmds.c` (depends on T054)
- [X] T104 [US3] Add the CLI subcommands `stats`, `stats reset`, and `monitor` (showing local operations and their results) in `software/client/padauk_prog/cli.py` (depends on T090)

**Checkpoint**: standalone mode works on `pdk-sim`; on F072 with the stub engine it blocks programming on a corrupted package and a failed self-test

---

## Phase 7: User Story 5 — Reading and copying an IC (Priority: P3)

**Goal**: reading into the client with a CRC; copying into a package with restoring calibration stubs separately for IHRC/ILRC/BG (`KEEP_VALUE`, `TARGET_FROM_HOST`, `SNAP_STANDARD`, `MEASURE_MASTER`), reference protection, handling of a measurement loop and foreign formats, the serial number not processed

**Independent Test**: `read` → the file = the programmed image; `copy --ihrc measure --allow-modify-master` from an 8 MHz reference (offset +3 %) → copies with offsets −4/0/+5 % within 1 % of the reference, the reference bit-for-bit unchanged

### Tests for User Story 5

- [X] T105 [P] [US5] Unit tests of forming a package from a dump in `software/firmware/tests/test_copy.c`: the fuse from the last word, `CREATED_BY_COPY`, `name` from `ext` or `copy <chip> <crc>`, stubs by mode for each type, `bg_restore`, `SNAP_STANDARD` for IHRC only (the `0xFF0000kk` encoding), serial-number bytes as is + `SERIAL_DUPLICATED`, `CALIB_UNRECOGNIZED` with an address, a protected fuse → `CHIP_PROTECTED`, an IC without writing → `OP_UNSUPPORTED`
- [X] T106 [P] [US5] `CopyJob` state machine unit tests in `software/firmware/tests/test_copy_job.c` with a fake engine: a measurement loop in the dump without `ALLOW_MASTER_LOOP` → `COPY_MASTER_UNCALIBRATED` with nothing written to storage, with permission and without `TARGET_FROM_HOST` → `BAD_PAYLOAD`; `MEASURE_MASTER`: the dump commit and `PENDING = 1` before the first erase, one rewrite per trace (n + 1 erases), a trace with no signal for 1 s → `COPIED` and continuing, `OP_CANCEL` in `MEASURE` → `RESTORE` → DONE, a failed `RESTORE` → `PENDING` stays; OTP → `MASTER_NOT_REWRITABLE`; without the flag → `MASTER_MODIFY_NOT_ALLOWED`; `COPY`/`PKG_BEGIN`/`PKG_DELETE` with `PENDING` → `MASTER_RESTORE_PENDING`
- [X] T107 [P] [US5] Contract test `software/client/tests/contract/test_read_copy.py`: `READ` → `data_crc32` = the CRC of the `RESULT_READ` data; `COPY` (`KEEP_VALUE`, `TARGET_FROM_HOST`, `SNAP_STANDARD`, BG restore/keep) → a new package, `EVT_PKG_STATE`, `RESULT.calib[]` and `warnings`; a protected model → `CHIP_PROTECTED`; `RESULT_READ` after `PKG_BEGIN` → `NOT_ALLOWED_NOW`
- [X] T108 [P] [US5] Contract test SC-009 `software/client/tests/contract/test_copy_measure.py`: a reference (offset +3 %) with an IHRC 8 MHz stub; `COPY` `MEASURE_MASTER` → the dump before and after is identical, erases +2; `PROGRAM` of three copies (−4 %, 0, +5 %) → within 1 % of the reference; `KEEP_VALUE` gives a deviation > 5 %; a variant IHRC + ILRC + BG → erases +3, both RC targets and BG restored; a variant with a trace the program never reaches → `COPIED` with a warning
- [X] T109 [P] [US5] Contract test SC-010 `software/client/tests/contract/test_copy_power_loss.py`: ≥ 100 power cuts at an arbitrary point of `COPY(MEASURE_MASTER)`, including between the measurements of two traces; after a restart `PENDING = 0` and the reference intact, or `PENDING = 1` and `RESTORE_MASTER` (from a PC and with a local start) restores the reference bit for bit
- [X] T110 [P] [US5] Contract test `software/client/tests/contract/test_copy_special.py`: a reference with a measurement loop — a local `COPY` and a `COPY` from a PC without permission → `COPY_MASTER_UNCALIBRATED`, `PKG_INFO` unchanged; with `ALLOW_MASTER_LOOP` + `TARGET_FROM_HOST` → a stub with the given target (FR-051); a reference with a constant write to `IHRCR` without a trace and with a free-pdk serial number → `OK`, the code as is, `warnings` = `CALIB_UNRECOGNIZED` + `SERIAL_DUPLICATED` (FR-052, FR-053)

### Implementation for User Story 5

- [X] T111 [US5] Implement the `READ` pipeline (data into the inactive slot, a `ReadResult` in RAM, the protection check per R10, `image_crc_raw/norm`) and the `RESULT_READ` command with invalidation on `PKG_BEGIN`/`READ`/`COPY` in `software/firmware/core/ops/op_read.c` and `software/firmware/core/ops/op_cmds.c` (depends on T082, T088, T056)
- [X] T112 [US5] Extend `software/firmware/hal/chip_engine.h` with the primitive `measure_freq(site, start_value, vdd_mV)` and implement it in `software/firmware/platform/host/chip_model.c` and `software/firmware/platform/stm32f072/chip_engine_stub.c` (`OP_UNSUPPORTED`) (depends on T079, T033)
- [X] T113 [US5] Implement the `COPY` operation per the `CopyJob` state machine (parsing the 14/46-byte `ext`, modes for IHRC/ILRC/BG, a measurement loop and `ALLOW_MASTER_LOOP`, `MEASURE_MASTER` with n + 1 erases and a 1 s trace timeout, the dump commit and `PENDING` before the first erase, a restore with verification, the final package with `CREATED_BY_COPY` and `name`, the `warnings` `SERIAL_DUPLICATED`/`CALIB_UNRECOGNIZED`/`MASTER_MODIFIED`) in `software/firmware/core/ops/op_copy.c` (depends on T080, T111, T112)
- [X] T114 [US5] Implement `RESTORE_MASTER`, blocking `PKG_BEGIN`/`PKG_DELETE`/`COPY` on `MASTER_RESTORE_PENDING`, and `STATUS.flags` in `software/firmware/core/ops/op_restore.c`, `software/firmware/core/package/upload.c`, `software/firmware/core/ops/op_cmds.c` (depends on T113)
- [X] T115 [US5] Implement `SNAP_STANDARD` in `PROGRAM`: a stub with the frequency `0xFF0000kk` → measure the copy at `kk`, round to {16, 8, 4, 2, 1 MHz, 500 kHz} if the deviation is ≤ 10 %, otherwise `kk` with `CALIB_VALUE_COPIED` in `software/firmware/core/package/effective_image.c` and `software/firmware/core/ops/op_program.c` (depends on T085, T080)
- [X] T116 [US5] Implement `CONFIG_GET`/`CONFIG_SET` (`cfg_copy_ihrc` 0/2, `cfg_copy_ilrc` 0, `cfg_copy_bg` 0/1; default 0, 0, 1) and the use of the configuration by the local `COPY` in `software/firmware/core/storage/config_cmds.c` and `software/firmware/core/ops/local_start.c` (depends on T101, T113)
- [X] T117 [US5] Extend the IC model: a "protected" mode (`:protected`), an OTP reference, a reference with a measurement loop, a constant write to `IHRCR` without a trace, the stdin command `swap-chip` in `software/firmware/platform/host/chip_model.c` and `software/firmware/platform/host/sim_main.c` (depends on T079)
- [X] T118 [US5] Add the CLI subcommands `read FILE -n NAME [-b]` (8-bit Intel HEX / raw bin, a `data_crc32` check), `copy [-n NAME] [--ihrc keep|target|snap|measure] [--ilrc keep|target|measure] [--bg keep|restore] [--ihrc-freq HZ] [--ilrc-freq HZ] [--calib-mv MV] [--allow-modify-master] [--name NAME]` (for `measure` — a confirmation with a warning about erase endurance; on `COPY_MASTER_UNCALIBRATED` — an interactive choice "cancel / set a target" and a retry with `ALLOW_MASTER_LOOP`; showing `SERIAL_DUPLICATED` and `CALIB_UNRECOGNIZED`), `restore-master`, `config copy [--ihrc keep|snap] [--bg keep|restore]` in `software/client/padauk_prog/cli.py` (depends on T090)

**Checkpoint**: US5 is verified on `pdk-sim`, including SC-009 and SC-010

---

## Phase 8: User Story 6 — Running an IC with data exchange (Priority: P3)

**Goal**: `RUN` with a voltage, a UART bridge both ways, stop; a dropped session stops `RUN`

**Independent Test**: the model in `pdk-sim` sends `"U"` + `"hello\n"` — the client shows `Connected @… baud` and the text; typed characters come back as an echo

### Tests for User Story 6

- [X] T119 [P] [US6] Contract test `software/client/tests/contract/test_run.py`: `OP_START(RUN, 5000)` → `EVT_UART_LINK`, `EVT_UART_RX`; `UART_TX` reaches the model; `UART_TX` outside `RUN` → `NOT_ALLOWED_NOW`; `OP_CANCEL` → the voltage removed, `OK`; a session drop → `HOST_DISCONNECTED`, the voltage removed

### Implementation for User Story 6

- [X] T120 [P] [US6] Add the UART bridge interface `software/firmware/hal/hal_uart.h` (start with baud-rate auto-detection, send, receive into a ring buffer, a "baud rate detected" event) and a "hello + echo" host implementation in `software/firmware/platform/host/hal_host_uart.c`
- [X] T121 [US6] Implement the `RUN` pipeline (VDD through `power_apply` with `param`/`run_vdd_mV`/5000, VPP = 0, the UART bridge, `EVT_UART_LINK`, `EVT_UART_RX`, stopping on `OP_CANCEL` and a dropped session) and the `UART_TX` command in `software/firmware/core/ops/op_run.c` (depends on T084, T120)
- [X] T122 [P] [US6] Implement the UART bridge for F072 (USART1 PB6/PB7, baud-rate auto-detection from the start bit, DMA receive, skipping the first `'U'`) in `software/firmware/platform/stm32f072/hal_f072_uart.c`
- [X] T123 [US6] Add the CLI subcommand `start [-r VDD]` (printing `EVT_UART_RX`, keyboard → `UART_TX`, Esc → `OP_CANCEL`) in `software/client/padauk_prog/cli.py` (depends on T090)

**Checkpoint**: all stories are verified on `pdk-sim`

---

## Phase 9: Polish & Cross-Cutting Concerns

- [X] T124 [P] Publish the protocol: `docs/protocol.md` = `contracts/protocol.md` + `contracts/package-format.md` + `contracts/chip-engine.md` with the sections "Versioning", "Status codes", "Mapping to easypdkprog"; a link from the root `README.md` (FR-029)
- [X] T125 [P] Add a firmware size check for each target in `software/firmware/CMakeLists.txt` (post-build `arm-none-eabi-size`; for F072 — an error if `.text + .data` > 44 KB, and the build options `PDK_FEATURE_MEASURE_MASTER`, `PDK_FEATURE_PATCH` to disable features, reflected in `INFO.capabilities`)
- [X] T126 [P] Add GitHub Actions `.github/workflows/ci.yml`: a `PLATFORM=host` build + `ctest` (Linux, Windows), a `PLATFORM=stm32f072` build with a size check, `pytest` unit + contract against `pdk-sim` (reduced fault-injection iterations; the full ones — `workflow_dispatch`)
- [X] T127 [P] Write `software/client/README.md` (installation, `pdkprog` examples for each subcommand, the mapping to `easypdkprog`, advice on the calibration and serial-number macros) and `software/firmware/README.md` (build, DFU, flash layout)
- [X] T128 Run scenarios 1–4 from `quickstart.md` and fix discrepancies in the documentation `specs/001-pc-programmer-protocol/quickstart.md`
- [ ] T129 Hardware validation of the debug target (F072 + the stub engine), the result in `specs/001-pc-programmer-protocol/hw-validation.md`: the firmware size and disabled features; the hardware voltage limit — the maximum VPP/VDD with the DAC at maximum and with a "hung" firmware (principle III); voltages removed right after reset/power-on; `pdkprog info/list/selftest` (accuracy ±2 % / ±30 mV and settling < 20 ms at every level, with a load simulating the target board); uploading a package and checking after power-off; 20 manual USB/power pulls during upload (the package is always `VALID`); a local start with a corrupted package and with a faulty/disconnected VPP; entropy quality (≥ 10⁵ generated numbers with no repeats, a proportion test); 8 KB upload speed ≤ 3 s (SC-003)
- [X] T130 Update `specs/001-pc-programmer-protocol/plan.md` and `research.md` if the implementation deviated from the decisions (firmware size → STM32F072CB, BLAKE2s → another hash function, etc.); record that SC-004 and SC-009 are verified on real ICs in feature `002-chip-engine`, and that the port to the primary MCU, the flash layout, and the number of slots belong to the MCU selection specification

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)** → no dependencies
- **Foundational (Phase 2)** → after Setup; **blocks all stories** (including the `core/power` voltage policy and `LIST_CHIPS`)
- **US1 (Phase 3)** → after Foundational
- **US2 (Phase 4)** → after US1 (the stored package, `validate`, `calib_sites`, `serial_sites`)
- **US4 (Phase 5)** → after US2 (the operation manager for `SELF_TEST`); `info`/`list` — only after Foundational
- **US3 (Phase 6)** → after US2 and US4 (a local start is blocked until a successful self-test)
- **US5 (Phase 7)** → after US2 (the trace detector, `PROGRAM`) and US3 (local copying, `local_start`)
- **US6 (Phase 8)** → after US2 (the operation manager); independent of US3–US5
- **Polish (Phase 9)** → after the required stories

### Story graph

```
Setup → Foundational → US1 → US2 ─┬─► US4 ─► US3 ─► US5
                                  └─► US6
```

### Within Each User Story

- Tests → fail → core implementation → commands → client/CLI
- Shared structures (`proto_defs.h`, `chip_table`, the HAL, `core/power`) are in Foundational; story tasks change other people's files only to register commands in the dispatcher and to extend interfaces (T112, T114, T115, T116 — explicitly marked)

---

## Parallel Example: User Story 1

```text
# Tests (different files):
T038 vectors   T039 test_package_validate.c   T040 test_sites.c   T041 test_journal.c   T042 test_upload.c
T043 test_ihex_package.py   T044–T048 contract tests
# Independent modules:
T049 header.c   T050 calib_sites.c   T051 serial_sites.c   T053 slots.c   T054 journal.c   T058 ihex.py
# Then: T052 → T055 → T056 → T057;  T059 → T060 → T061
```

## Parallel Example: User Story 2

```text
T062–T069 unit tests   T070–T078 contract tests   T079 chip_model.c   T083 serial_rng.c
# Then: T080 → T081 → T082;  T084;  T085, T086, T087 → T088 → T089;  T090 → T091, T092
```

## Parallel Example: US4 and US6 after US2

```text
Developer A: T093–T095 → T096 → T097 → T098 (US4) → US3
Developer B: T119, T120, T122 → T121 → T123 (US6)
```

---

## Implementation Strategy

### MVP First (User Story 1)

1. Phase 1 Setup → Phase 2 Foundational (with tests, including the voltage policy)
2. Phase 3 US1 → **STOP and VALIDATE**: T044–T048 on `pdk-sim`, then uploading to F072
3. Result: reliable uploading and storing of a package — the foundation of standalone operation

### Incremental Delivery

1. US1 → intact upload (MVP)
2. US2 → programming, verification, `PATCH` on the model (on hardware — together with `002-chip-engine`)
3. US4 → `info` / `list` / `selftest`
4. US3 → standalone mode
5. US5 → reading/copying
6. US6 → running with UART

### Notes

- [P] = different files, no dependencies on unfinished tasks
- Every story ends with a checkpoint with green tests
- Commit after each task or logical group
- The real PADAUK algorithms (the `chip_engine` for F072) are feature `002-chip-engine`; here — a stub and a model
