# Quickstart: verifying the PC ⇄ programmer protocol

Scenarios that prove the feature works. Paths follow the structure in [plan.md](plan.md).
Formats are in [contracts/](contracts/).

## Prerequisites

- CMake ≥ 3.20, a host C11 compiler gcc/clang (MinGW-w64 on Windows; MSVC is not supported: GNU extensions
  in the core), `arm-none-eabi-gcc` ≥ 12 (for the firmware)
- Python ≥ 3.8, `pip install -e software/client[dev]` (pyserial, pytest)
- For the hardware scenarios: the debug target is an Easy PDK board (STM32F072), a PFS154 in the socket

## 1. Core unit tests (no hardware)

```sh
cmake -S software/firmware -B build/host -DPLATFORM=host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Expectation: all tests pass, including COBS, CRC-32 (reference `"123456789"` → `0xCBF43926`), frame
parsing, package validation on the vectors from `software/tests/vectors/`, atomicity of the log and slots.

## 2. The client against the simulator

```sh
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154 &
pdkprog --port tcp://127.0.0.1:5555 info
pdkprog --port tcp://127.0.0.1:5555 list
pdkprog --port tcp://127.0.0.1:5555 -n PFS154 write software/tests/vectors/examples/helloworld_pfs154.ihx
```

Expectation: `info` shows protocol 1.0 and `max_package_size`; `list` — the table from the firmware;
`write` shows the stages `PKG_CHECK → ID → ERASE → BLANK_CHECK → WRITE → VERIFY → CALIBRATE → PKG_CHECK →
[FUSE] → FINAL_VERIFY` (`FUSE` only if the package sets a fuse; `helloworld_pfs154` has none), progress
at least once per second, and the summary `Programming done` with the line `Calibration IHRC: target 8000000 Hz, value
0x…, measured … Hz [OK]`. The CRC shown by the client after upload matches `pdkprog pkg info`.
The order `-n PFS154 write`, as in `easypdkprog`, and `write -n PFS154` are equivalent.

## 3. Contract tests with fault injection (SC-001, SC-002, SC-007)

```sh
pytest software/client/tests/contract --sim build/host/pdk-sim
```

| Test | What it does | Expectation |
|---|---|---|
| `test_frame_corruption` | 1000 uploads of the maximum package, ≥ 1 % of frames in each direction corrupted/lost/duplicated | 100 % of the stored packages are identical to the original |
| `test_stored_corruption` | corrupts each byte of the stored package in turn in the emulated flash | `PROGRAM` always → `STORE_CORRUPTED`, the IC model is unchanged |
| `test_power_loss` | ≥ 100 "power cuts" at an arbitrary flash write operation during upload | after a "restart" the active package is either the old or the new one, `pkg_state = VALID` |
| `test_duplicate_seq` | repeats a request with the same `seq` | the command is executed once, the response is the same |
| `test_busy_cancel` | commands during `PROGRAM`, then `OP_CANCEL` | `BUSY` for the forbidden ones, `CANCELLED` with the stage |
| `test_disconnect` | ≥ 100 times closes the port at an arbitrary moment of `PROGRAM` (SC-008) | `PROGRAM` completes, the model contents = as without the drop, `OP_RESULT(0)` after reconnecting — `OK` |
| `test_run` | `RUN` with a "hello + echo" model: `UART_TX`, `OP_CANCEL`, a closed port | `EVT_UART_LINK` / `EVT_UART_RX`, echo; `OP_CANCEL` → `OK`; a drop → `HOST_DISCONNECTED`; voltages removed |
| `test_otp_retry` | ≥ 100 "power cuts" at an arbitrary point of `PROGRAM` of the OTP model PMS150C, keeping the model state, then a repeated `PROGRAM` with the same and with a "different" simulator (a separate `--flash`, the same package) (SC-012, FR-046) | `OK` + `RESUMED` or `CHIP_NOT_BLANK` with the model unchanged; no extra zeros; fully programmed → `CHIP_ALREADY_PROGRAMMED`, counters unchanged; an already written serial number is kept; a blank stub gets a new random number |
| `test_calib_fail` | a model with a "faulty" oscillator → `PROGRAM`; a local start; from a PC with `CALIB_FINALIZE_ON_FAIL`; then a "repair" and a repeated `PROGRAM` (SC-013) | `CALIB_FAILED`, nothing written after the loop, the fuse is not written; the local start ignores the flag; with the flag — `CALIB_FORCED`; the repeated write → `OK` + `RESUMED` |
| `test_copy_measure` (several traces) | a reference with IHRC + ILRC + BG in `MEASURE_MASTER` for both RC oscillators; a variant with a trace the program never reaches (`pdk-sim` `halt=ADDR`) | reference erases +3, both RC targets restored, BG — a stub; the unreachable trace → `COPIED` with a warning, the rest processed |
| `test_copy_power_loss` | ≥ 100 power cuts at an arbitrary point of `COPY(MEASURE_MASTER)` with two traces, including between measurements (SC-010) | `PENDING = 0` and the reference intact, or `PENDING = 1` and `RESTORE_MASTER` (from a PC and locally) restores the reference bit for bit |
| `test_copy_special` (loop in the reference) | a reference with unfinished calibration (a measurement loop): a local `COPY`; `COPY` from a PC without permission; then with `ALLOW_MASTER_LOOP` + `TARGET_FROM_HOST` (FR-051) | locally and without permission — `COPY_MASTER_UNCALIBRATED`, `PKG_INFO` unchanged; with permission — a new package with a stub in place of the loop, the target = the given one |
| `test_copy_special` (a foreign format, a serial number) | a reference with a constant write to `IHRCR` without a free-pdk trace and with a free-pdk serial number (FR-052, FR-053) | `COPY` → `OK`, the code copied as is, `warnings` = `CALIB_UNRECOGNIZED` (+ the address) and `SERIAL_DUPLICATED` |
| `test_verify_final` (VERIFY taking stubs into account) | `VERIFY` of a calibrated IC with a serial number; ICs with unfinished calibration (FR-047) | the first → `OK`, `calib[].value` and `serial_written` from the IC; the second → `VERIFY_FAILED`, `site_status = FAILED` |
| `test_verify_final` (FINAL_VERIFY, streaming CRC) | the model "weakly" writes the stub-finishing word or the fuse (FR-048); `pdk-sim --corrupt-flash-during-write` (FR-049); a package without calibration stubs (FR-050) | `VERIFY_FAILED` at `FINAL_VERIFY` with an address; `STORE_CORRUPTED` before the fuse is written; `PKG_COMMIT.calib_sites = 0` and a client warning |
| `test_upload` (invalid vectors) | vectors with header, CRC, reserved-area, fuse, and stub errors | the exact codes from the contract and `addr` |
| `test_latency` | measures the time of every RSP, including `PKG_DATA` at a flash page boundary, `PKG_COMMIT`, and a log write that moves to another page (FR-024) | all responses ≤ 100 ms |
| `test_selftest` | `pdk-sim --chip none` → `SELF_TEST`; `--chip PFS154` → `SELF_TEST`; restarting the simulator | `OK` with all levels; `SELFTEST_CHIP_PRESENT` with no levels above probe; after start `STATUS.selftest_state = 2` |
| `test_power_fault` | `pdk-sim --power-fault vpp:slow\|vpp:stuck\|vdd:low` → `PROGRAM`, `SELF_TEST` (SC-011) | `VPP_FAULT`/`VDD_FAULT` ≤ 20 ms after the change, `RESULT.v_*` filled in, the IC model unchanged; after a start with the fault, a local start → `SELF_TEST`, not `PROGRAM` |

## 4. Copying with calibration restore (simulator, SC-009, SC-010)

```sh
# terminal 1 (the simulator accepts commands from stdin):
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154:offset=+3% --chip-state master.bin
# terminal 2:
pdkprog --port tcp://127.0.0.1:5555 -n PFS154 write software/tests/vectors/examples/helloworld_pfs154.ihx   # the reference, IHRC 8 MHz
pdkprog --port tcp://127.0.0.1:5555 copy --ihrc measure --allow-modify-master
# in the pdk-sim console (stdin): swap-chip PFS154:offset=-4%     — "insert" a blank copy IC
pdkprog --port tcp://127.0.0.1:5555 program
pytest software/client/tests/contract/test_copy_measure.py software/client/tests/contract/test_copy_power_loss.py --sim build/host/pdk-sim
```

Expectation: `copy` asks for confirmation (or `--yes`), shows the stages `ID → READ → STORE → MEASURE → RESTORE →
STORE`, and in the summary `IHRC: master value 0x…, target ≈8000000 Hz (measured on the master) [OK]` and the note
`MASTER_MODIFIED`;
the reference dump before and after is identical; `program` of the copy with a −4 % offset gives a frequency within 1 % of the reference.
A power cut during `MEASURE`/`RESTORE` → after a restart `pdkprog status` shows
`MASTER_RESTORE_PENDING`, and `pdkprog restore-master` restores the reference.

## 5. On hardware (debug target STM32F072 + PFS154)

```sh
cmake -S software/firmware -B build/f072 -DPLATFORM=stm32f072 -DCMAKE_TOOLCHAIN_FILE=$PWD/software/firmware/cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build/f072          # the build itself checks .text + .data ≤ 44 KB (cmake/check_size.cmake)
dfu-util -d 0483:df11 -a 0 --dfuse-address 0x08000000 -D build/f072/pdkprog.bin
pdkprog info
pdkprog selftest          # without an IC: a table of VDD/VPP levels, set / measured / time
pdkprog -n PFS154 write helloworld_pfs154.ihx
pdkprog stats
```

Then **without a PC** (powered from a USB charger): insert a PFS154 and run a local start three times (on the current board — with the button) for three ICs.
Connect the PC: `pdkprog stats` → `ok = 4`; for a firmware with a serial-number stub — `pdkprog verify` on each IC shows different serial numbers.

Programming real ICs depends on feature `002-chip-engine`; until then scenario 5 verifies only the
protocol, storage, and the standalone integrity check (with the stub engine).
