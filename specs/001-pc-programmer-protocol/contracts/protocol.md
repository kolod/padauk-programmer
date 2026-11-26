# Contract: PC ⇄ programmer protocol (protocol version 1.0)

The transport is USB CDC ACM. The port speed is ignored. All multi-byte fields are little-endian.
CRC is CRC-32/ISO-HDLC (`zlib.crc32`). The firmware package format is in [package-format.md](package-format.md).

## 1. Frame

On the wire every frame is a COBS-encoded block followed by a `0x00` byte. After COBS decoding:

| Off | Type | Field | Description |
|---|---|---|---|
| 0 | `u8` | `type` | `0x01` REQ (client→programmer), `0x02` RSP, `0x03` EVT, `0x04` NAK |
| 1 | `u8` | `flags` | 0 (reserved) |
| 2 | `u16` | `seq` | REQ/RSP — request number; EVT — the programmer's event counter; NAK — `0xFFFF` |
| 4 | `u16` | `code` | command ID (REQ/RSP) or event ID (EVT) |
| 6 | `u16` | `status` | RSP — result code (§6); REQ/EVT/NAK — 0, or `FRAME_ERROR` for NAK |
| 8 | `u16` | `len` | length of `payload`, ≤ `max_payload` |
| 10 | `u8[len]` | `payload` | |
| 10+len | `u32` | `crc32` | CRC of bytes `0 … 9+len` |

Size of a decoded frame: 14 + `len`. The protocol's `max_payload` is 1024; the programmer reports its own
(512 on F072), and the client MUST use the smaller of the two.

**These frame fields and the `GET_INFO`/`HELLO` commands are frozen for all major versions.**

## 2. Reliability (stop-and-wait)

1. The client has at most one outstanding REQ. Every **new** request uses `seq = (prev + 1) mod 65536`.
2. The programmer receives a frame and checks COBS, length, and CRC. An error → `NAK` (nothing is executed).
3. If `seq` equals the `seq` of the previously accepted request, the programmer resends the **stored response**
   without executing anything.
4. The client repeats the same REQ (with the same `seq`) if it received a `NAK`, received a corrupted RSP, or
   received no RSP within **300 ms**. After **5** failed retries the session is considered lost.
5. The programmer MUST answer every REQ within ≤ **100 ms** of receiving it (FR-024).
6. Opening/closing the port (a DTR change) resets the response cache and the session.
7. EVT frames may appear at any moment between RSPs; the client tells them apart by `type`.

## 3. Session and versions

- Before a successful `HELLO` in the current port session, only `HELLO` and `GET_INFO` are allowed; others → `NO_SESSION`.
- `HELLO` with an incompatible major version → `PROTO_VERSION` (the payload carries the same information as `GET_INFO`);
  the session is not opened.
- An unknown `code` → `UNKNOWN_COMMAND`, the session is not dropped (new minor commands).
- The minor version grows when commands/events/fields are added at the end of the payload; the client MUST ignore
  extra bytes at the end of a response payload. The major version grows on incompatible changes.

## 4. Commands

Notation: `→` is the request payload, `←` is the response payload when `status = OK`. On error the response payload
is empty unless stated otherwise.

### Session and information

| Code | Name | → | ← | During an operation |
|---|---|---|---|---|
| `0x0001` | `HELLO` | `u8 proto_major, u8 proto_minor, char client[≤32]` | `INFO` | ✓ |
| `0x0002` | `GET_INFO` | — | `INFO` | ✓ |
| `0x0003` | `GET_STATUS` | — | `STATUS` | ✓ |
| `0x0004` | `LIST_CHIPS` | `u8 first_index` | `u8 total, u8 count, CHIP_DESC[count]` | ✗ |

**`INFO`**:

| Type | Field |
|---|---|
| `u8, u8` | `proto_major`, `proto_minor` |
| `u16` | `max_payload` |
| `u32` | `max_package_size` — the largest package that can be written (slot capacity; F072 — 8192) |
| `u16` | `max_read_words` — the largest number of words for `READ` (F072 — 4096) |
| `u32` | `capabilities` (bit0 standalone mode, bit1 calibration, bit2 RUN/UART, bit3 COPY, bit4 VDD/VPP measurement, bit5 `SELF_TEST`, bit6 `PATCH`, bit7 supply calibration `CAL_*`, bit8 manual supply control `SUPPLY_*`, bit9 `COPY` with `MEASURE_MASTER` — a build may disable it, then mode 3 → `OP_UNSUPPORTED`) |
| `char[16]` | `platform` (e.g. `"stm32f072c8"`) |
| `char[24]` | `firmware_version` |
| `u8` | `chip_table_size` |

**`STATUS`**:

| Type | Field |
|---|---|
| `u8` | `state`: 0 idle, 1 upload, 2 operation, 3 running (RUN) |
| `u16` | `op_id` of the current/last operation (0 — none yet) |
| `u8` | `op_type`, `u8 op_stage`, `u8 op_percent` |
| `u8` | `pkg_state`: 0 empty, 1 valid, 2 corrupted |
| `u32` | `pkg_crc32` (0 if empty) |
| `u32, u32` | `ok_count`, `fail_count` |
| `u8` | `flags`: bit0 `MASTER_RESTORE_PENDING` (the reference was modified and not restored), bit1 `LOCAL_START_BLOCKED` (a local programming start is replaced by the self-test) |
| `u8 × 3` | `copy_cfg` — calibration modes for the local `COPY`: `mode_ihrc`, `mode_ilrc`, `bg_restore` (see `CONFIG_SET`) |
| `u8` | `selftest_state`: 0 not run, 1 running, 2 passed, 3 failed, 4 IC connected |

**`CHIP_DESC`** (48 bytes):

| Type | Field |
|---|---|
| `u8` | `index` |
| `u16` | `chip_id12` |
| `u8` | `family` (`0x12`,`0x21`,`0x22`,`0x31`,`0xF1`,`0xF2`) |
| `u16` | `code_words` |
| `u8` | `code_bits` |
| `u16` | `ram_bytes` |
| `u8` | `ops`: bit0 read, bit1 write, bit2 erase, bit3 calibrate |
| `u16, u16` | `reserved_start`, `reserved_end` (inclusive; `0xFFFF` — none) |
| `u8` | `reserved_first_word` (1 — address 0 is reserved) |
| `char[33]` | names separated by `/`, e.g. `"PMS150C/PMS15A"`, NUL-padded (≤ 32 characters + NUL) |

### Firmware package and storage

| Code | Name | → | ← | During an operation |
|---|---|---|---|---|
| `0x0010` | `PKG_BEGIN` | `Header` (128 B) | `u32 package_size` | ✗ |
| `0x0011` | `PKG_DATA` | `u32 offset, u8 data[]` | `u32 received` | ✗ |
| `0x0012` | `PKG_COMMIT` | — | `u32 package_crc32, u8 calib_sites, u8 serial_sites, u16 effective_words` (`calib_sites = 0` → the client shows a warning, FR-050) | ✗ |
| `0x0013` | `PKG_ABORT` | — | — | ✗ |
| `0x0014` | `PKG_INFO` | — | `u8 pkg_state, u32 package_crc32, Header` | ✓ |
| `0x0015` | `PKG_READ` | `u32 offset, u16 len` | `u8 data[len]` | ✗ |
| `0x0016` | `PKG_DELETE` | — | — | ✗ |
| `0x0017` | `PKG_CHECK` | — | `u8 pkg_state, u32 computed_crc32` | ✗ |
| `0x0018` | `STATS_GET` | — | `u32 ok, u32 fail` | ✓ |
| `0x0019` | `STATS_RESET` | — (reset ok/fail) | — | ✗ |
| `0x001A` | `CONFIG_GET` | — | `u8 mode_ihrc, u8 mode_ilrc, u8 bg_restore` | ✓ |
| `0x001B` | `CONFIG_SET` | `u8 mode_ihrc` (0 or 2), `u8 mode_ilrc` (0), `u8 bg_restore` (0/1); default `0, 0, 1` | — | ✗ |

Upload rules:

- `PKG_BEGIN` starts an upload into the inactive slot; the previous package stays active.
- `PKG_BEGIN` contains a copy of the header for early validation only. The package itself is transferred **in full** (with
  the header) through `PKG_DATA`, starting at `offset = 0`. `PKG_DATA.offset` MUST
  equal the number of bytes already received; any other value → `PKG_OFFSET`, with `u32 expected` in the
  error payload. The first 128 bytes of data MUST match the header from `PKG_BEGIN`, otherwise
  `PKG_HEADER_INVALID`.
- `PKG_COMMIT` is allowed only once `package_size` bytes have been received. On a validation error the response payload is
  `u16 addr` (the address of the offending word or `0xFFFF`). On success the package becomes active.
- Any error, `PKG_ABORT`, or a dropped session cancels the upload; the active package is unchanged.

### Supply calibration

Calibration of VDD/VPP against an external voltmeter. The IC and the target board MUST be disconnected: the client
warns and asks for confirmation before `CAL_START`. Available if `INFO.capabilities` bit7 is set.

| Code | Name | → | ← | During an operation |
|---|---|---|---|---|
| `0x0040` | `CAL_START` | — | `u16 vdd_set_mV, u16 vpp_set_mV, u16 vdd_raw_mV, u16 vpp_raw_mV` | ✗ |
| `0x0041` | `CAL_SET` | `u16 vdd_true_mV, u16 vpp_true_mV` | `u16 vdd_target_mV, u16 vpp_target_mV, u16 vdd_meas_mV, u16 vpp_meas_mV, i8 cal[4]` | ✗ |
| `0x0042` | `CAL_CONFIRM` | `u8 accept` (1 — save, 0 — discard) | — | ✗ |
| `0x0043` | `CAL_GET` | — | `u8 valid, i8 cal[4], u8 state` (0 no calibration, 1 voltages applied, 2 verification) | ✓ |
| `0x0044` | `CAL_CLEAR` | — | — | ✗ |

Sequence:

1. `CAL_START`: the programmer applies VDD and VPP simultaneously at 90 % of each rail's limit (below the saturation of the
   output stage), without a tolerance check, and returns the set values and the uncalibrated measurements.
   If a rail does not rise to at least half of the set value → `VDD_FAULT`/`VPP_FAULT` (`u8 rail, u16 set_mV,
   u16 meas_mV`), and the voltages are removed.
2. The user measures both voltages at the outputs and enters them in the client; `CAL_SET` computes for each
   rail two coefficients in parts per thousand: measurement (`true / ADC reading`) and output (`true / set`).
   A deviation over ±12.7 % → `BAD_PAYLOAD` (`u8 rail`), the voltages stay on — the values can be entered
   again. Otherwise the programmer, by the rule of §6.1 with the new coefficients, applies exactly 6 V and 12 V (no more than
   95 % of the rail limit) and returns the targets, the calibrated measurements, and `cal[4]`.
3. The user checks the voltmeter and confirms: `CAL_CONFIRM(1)` writes the coefficients to the state log,
   `CAL_CONFIRM(0)` discards them; in both cases the voltages are removed.

`cal[4]`: `i8` in parts per thousand (±127) — VDD measurement, VPP measurement, VDD output, VPP output. True voltage =
ADC reading · (1000 + measurement) / 1000; the value set for the hardware = required · 1000 / (1000 + output). Without
calibration all are zero. A dropped session, `CAL_CONFIRM(0)`, or 5 minutes without `CAL_*` commands → the voltages are removed and the
previous calibration stays in effect. `CAL_SET`/`CAL_CONFIRM(1)` in the wrong state → `NOT_ALLOWED_NOW`;
`CAL_START` without a supply circuit → `OP_UNSUPPORTED`.

### Manual supply control

Arbitrary VDD/VPP for checking the supply circuit with a voltmeter. The IC and the target board MUST be
disconnected: the client warns and asks for confirmation before the first `SUPPLY_SET`. Available if
`INFO.capabilities` bit8 is set.

| Code | Name | → | ← | During an operation |
|---|---|---|---|---|
| `0x0045` | `SUPPLY_SET` | `u16 vdd_mV, u16 vpp_mV` (0 — rail off) `[, u8 flags]` | `SUPPLY_STATE` | ✗ |
| `0x0046` | `SUPPLY_GET` | — | `SUPPLY_STATE` | ✗ |

`SUPPLY_STATE`: `u16 vdd_set_mV, u16 vpp_set_mV, u16 vdd_meas_mV, u16 vpp_meas_mV, u16 vdd_min_mV,
u16 vdd_max_mV, u16 vpp_min_mV, u16 vpp_max_mV` — the set values, the calibrated measurements (the mean of 32 over
~2.5 ms), and their spread: a large spread means ripple, a small spread with a noticeable deviation means a constant offset.

`flags`: bit0 `NO_CHECK` — apply the voltages without a tolerance check (only a 10 ms pause), to compare with a
voltmeter a rail that does not fit the tolerance; `*_FAULT` then never occurs. Other bits → `BAD_PAYLOAD`.

- `SUPPLY_SET` applies the voltages by the rule of §6.1 with the stored calibration and a 10 ms pause; VPP
  is lowered before VDD and raised after it. If a rail did not settle → safe state,
  `VDD_FAULT`/`VPP_FAULT` (`u8 rail, u16 set_mV, u16 meas_mV`). Without calibration the error of the output
  stage may exceed the ±2 % tolerance — that is also a `*_FAULT`.
- A value above the rail limit → `BAD_PAYLOAD` (`u8 rail`), the voltages do not change. `SUPPLY_SET(0, 0)` removes
  both.
- A dropped session, `CAL_START`, or 5 minutes without `SUPPLY_*` commands → the voltages are removed (`SUPPLY_GET` extends the
  time, so a client that shows measurements keeps them). During calibration `SUPPLY_SET` → `NOT_ALLOWED_NOW`;
  without a supply circuit both commands → `OP_UNSUPPORTED`.

### IC operations

| Code | Name | → | ← | During an operation |
|---|---|---|---|---|
| `0x0020` | `OP_START` | `u8 op_type, u8 op_flags, u16 chip_id12, u16 param, u8 ext[]` | `u16 op_id` | ✗ (`BUSY`) |
| `0x0021` | `OP_CANCEL` | `u16 op_id` | — | ✓ |
| `0x0022` | `OP_RESULT` | `u16 op_id` (0 — the last one) | `RESULT` | ✓ |
| `0x0023` | `RESULT_READ` | `u32 offset, u16 len` | `u8 data[len]` | ✗ |
| `0x0030` | `UART_TX` | `u8 data[]` | — | ✓ (RUN only) |

`op_type`:

| Code | Operation | `chip_id12` | `param` | Stages (`op_stage`) |
|---|---|---|---|---|
| `0x01` | `PROBE` | — | — | `ID` |
| `0x02` | `READ` | type to read (0 — from `PROBE`, if unambiguous) | — | `ID`, `READ` |
| `0x03` | `ERASE` | type (0 — from the active package) | — | `ID`, `ERASE`, `BLANK_CHECK` |
| `0x04` | `BLANK_CHECK` | type (0 — from the package) | — | `ID`, `BLANK_CHECK` |
| `0x05` | `PROGRAM` | — (from the package) | — | `PKG_CHECK`, `ID`, `ERASE`, `BLANK_CHECK`, `WRITE`, `VERIFY`, `CALIBRATE`, `FUSE`, `FINAL_VERIFY` |
| `0x06` | `VERIFY` | — (from the package) | — | `PKG_CHECK`, `ID`, `VERIFY` (taking stubs into account, see below) |
| `0x07` | `COPY` | reference type (0 — from `PROBE`) | — | `ID`, `READ`, `STORE`, [`MEASURE` × n, `RESTORE`, `STORE`] |
| `0x08` | `RUN` | — | `vdd_mV` (0 — the package's `run_vdd_mV` or 5000) | `RUN` |
| `0x09` | `RESTORE_MASTER` | — (from the active dump package) | — | `ID`, `ERASE`, `WRITE`, `VERIFY`, `FUSE` |
| `0x0A` | `PATCH` | IC type (mandatory) | — | `ID`, `READ`, `WRITE`, `VERIFY` |
| `0x0B` | `SELF_TEST` | — | — | `ID` (probe), `SELF_TEST` |

`ext` for `COPY` (14 or 46 bytes; if absent — `KEEP_VALUE, KEEP_VALUE, bg_restore = 0`):

| Type | Field |
|---|---|
| `u8` | `mode_ihrc`: 0 `KEEP_VALUE`, 1 `TARGET_FROM_HOST`, 2 `SNAP_STANDARD`, 3 `MEASURE_MASTER` |
| `u8` | `mode_ilrc`: 0 `KEEP_VALUE`, 1 `TARGET_FROM_HOST`, 3 `MEASURE_MASTER` (2 → `BAD_PAYLOAD`) |
| `u8` | `bg_restore`: 0 — carry over the reference value, 1 — restore the stub with a fixed target |
| `u8` | `copy_flags`: bit0 `ALLOW_MODIFY_MASTER` (mandatory if at least one mode = 3), bit1 `ALLOW_MASTER_LOOP` — allow copying a reference that contains measurement loops (from a PC only; for every type where a loop is found the mode MUST be `TARGET_FROM_HOST`, otherwise `BAD_PAYLOAD`) |
| `u32` | `target_ihrc_hz` (mode 1 for IHRC) |
| `u32` | `target_ilrc_hz` (mode 1 for ILRC) |
| `u16` | `mV` — the voltage for calibrating the copies and measuring the reference; 0 → 5000 |
| `char[32]` | `name` — the name of the new package (optional; absent or empty → `"copy <chip_name> <crc>"`) |

`MEASURE_MASTER` measures each stub with a separate rewrite of the reference: the number of reference erases =
the number of stubs in mode 3 + 1. A stub with no signal on PA3 for 1 s → the reference value, `site_status =
COPIED`, warning `CALIB_VALUE_COPIED`; the remaining stubs are still processed.

Mode 3 for an OTP reference → `MASTER_NOT_REWRITABLE`; without `ALLOW_MODIFY_MASTER` → `MASTER_MODIFY_NOT_ALLOWED`;
`COPY` with `MASTER_RESTORE_PENDING` → `MASTER_RESTORE_PENDING` (run `RESTORE_MASTER` first).
`COPY` of an IC without a write operation in `CHIP_DESC.ops` → `OP_UNSUPPORTED`.

**A measurement loop in the reference (FR-051).** After `READ` the programmer searches the dump not only for traces but also for
measurement loops (the same opcodes the programmer / `easypdkprog` writes, with an arbitrary value in
`MOV A,#…`). If at least one is found and `ALLOW_MASTER_LOOP` is absent → `COPY_MASTER_UNCALIBRATED`, stage `READ`,
**nothing is written to storage** (the active package is unchanged); `RESULT.calib[]` contains every loop
(`site_status = MASTER_LOOP`, `kind`, the address of the first one in `fail_addr`). A local start ends
with this code and the failure signal. A client on a PC shows the list and asks the user: "cancel" or "set
a target" → a repeated `OP_START(COPY)` with `ALLOW_MASTER_LOOP` and `TARGET_FROM_HOST` for those types (the reference is
read again); in place of the loop a stub with the given target is restored, and the remaining traces follow their own
modes.
The mode algorithm is research R9. `RESTORE_MASTER` successfully clears `MASTER_RESTORE_PENDING`.

#### `SELF_TEST` (self-test without an IC)

1. `PROBE` at VPP 4.5 V / VDD 2.0 V; if an IC answers → `SELFTEST_CHIP_PRESENT`, higher voltages are not applied.
2. VDD (VPP = 0), then VPP (VDD = 0): each level — all distinct values of the corresponding rail from the IC table in ascending order, with 0 V at the end of each rail. Each level is set by the rule of §6.1.
3. If any level is not reached → `SELFTEST_FAILED` (the remaining levels are not run, `safe_state`).
4. `RESULT.levels[]` — one record per level; `RESULT.status = OK` if all levels passed.

The result is kept in RAM and shown in `STATUS.selftest_state`; the self-test runs automatically at every start (`source = 1`).

`op_flags` (only for `PROGRAM`, combined with the package flags by **OR**, only toward "skip"):
bit0 `SKIP_ERASE`, bit1 `SKIP_BLANK_CHECK`, bit2 `SKIP_VERIFY`, bit3 — reserved (0),
bit4 `CALIB_FINALIZE_ON_FAIL` (from a PC only: on failed calibration, finish the stub with the best
value found), bits 5–7 — reserved (0). Skipping the calibration of an existing stub is not possible.
`SKIP_VERIFY` skips only the `VERIFY` stage; `FINAL_VERIFY` (FR-048) always runs.

**Retry (FR-044).** It depends only on the IC contents and the active package — not on the session,
time, or programmer. FLASH: an ordinary `PROGRAM` (erase + full write). An OTP that is not blank: the
`BLANK_CHECK` stage checks "blank **or** compatible with the effective image" — for each word
`(content AND target) == target`, where:
- a calibration stub: the allowed states are "blank", "measurement loop", "finished with an arbitrary `k`";
- a serial-number stub: all 8 words blank → a new random number will be written; all 8 are `RET #imm` → the serial
  number is **read from the IC** and kept; any other state → incompatible.
- a calibration stub between the loop and the finished form (interrupted while being finished) belongs to none
  of the three states → incompatible.

Compatible → write the rest, calibrate the unfinished stubs, verify; `warnings.RESUMED`,
`RESULT.serial_written` — the actual number. Nothing to write and the calibrations are finished →
`CHIP_ALREADY_PROGRAMMED`, the counters do not change. Incompatible → `CHIP_NOT_BLANK` with
`fail_addr/expected/actual`, with nothing written.

**Serial numbers (FR-046).** For each IC the programmer generates a random 64-bit number from the
hardware entropy source (research R18) and writes it into the serial-number stub; there are no counters or
parameters. A number taken from the IC on a retry is kept. `COPY` does not process the serial number
(FR-052): `warnings.SERIAL_DUPLICATED` is a reminder that the reference's number, if it has one, has been duplicated.

**Verification taking stubs into account (FR-047)** — for the `VERIFY` operation and the `VERIFY` stage in `PROGRAM`:

| Place | Accepted |
|---|---|
| calibration stub | `VERIFY` (operation): the finished form with any `k`; a measurement loop or unfinished stub → `VERIFY_FAILED` with `fail_addr` and `calib[].site_status = FAILED`. `VERIFY` stage in `PROGRAM` (before calibration): the measurement loop |
| serial-number stub | any 8 `RET #imm`; the number is returned in `RESULT.serial_written` |
| other words | an exact match (bits above `code_bits` are ignored) |

`RESULT.calib[]` for `VERIFY` contains the actual `k` (`value`) from the IC.

**Final check (FR-048).** The `FINAL_VERIFY` stage is the last one in `PROGRAM`, after `FUSE`: a full
read of user memory and an **exact** comparison with the effective image, into which the actual
`k` of each stub, the actual serial number, and the written fuse have been substituted. A mismatch → `VERIFY_FAILED` with
`fail_addr/expected/actual`. `RESULT.image_crc_raw/norm` are computed from this same read. It is not
run if the operation stopped earlier (for example `CALIB_FAILED` without `CALIB_FINALIZE_ON_FAIL`).

**Re-checking the package during programming (FR-049).** During `WRITE` the package CRC is computed on the fly from
the same data that is read from flash for writing, and compared with `package_crc32` after `WRITE`
completes; before `FUSE` the `PKG_CHECK` check is repeated. A mismatch → `STORE_CORRUPTED`, the fuse is not
written (for OTP the IC stays usable for a retry if the package is restored).

**Failed calibration (FR-043).** `CALIB_FAILED` → nothing more is written: the stub stays a
measurement loop, the following calibrations and the fuse are not performed, `fail_count` +1 (the serial number,
if already written, stays spent). With `CALIB_FINALIZE_ON_FAIL` (only `source = 0`) the stub is finished with the best value,
the remaining stages run, `status = CALIB_FAILED`, `warnings.CALIB_FORCED`. A local start
(`source = 1`) ignores the flag.
`SKIP_BLANK_CHECK` for an OTP IC → `PKG_OTP_BLANK_CHECK_REQUIRED` (programming does not start).
Appending to an already programmed IC is possible only with the `PATCH` operation.

#### `PATCH` (appending individual words)

`ext` for `PATCH` (`chip_id12` is mandatory, the package is not used):

| Type | Field |
|---|---|
| `u8` | `patch_flags`: bit0 `CHECK_ONLY` (only check, do not write), bit1 `ALLOW_SECURITY_FUSE` |
| `u8` | `expect_mode`: 0 `RAW` — `expected_crc32` of the exact sum; 1 `NORMALIZED` — `expected_crc32` of the normalized sum; 2 `ACTIVE_PACKAGE` — the programmer itself computes the normalized sum from the active package (`expected_crc32` is ignored) |
| `u32` | `expected_crc32` |
| `u8` | `count` — 1…64 (may be 0 for `CHECK_ONLY`) |
| `count` × `{u16 addr, u16 value}` | the word address and the **new full value** (bits above the core width are ignored) |

Stages: `ID` → `READ` (all user memory) → checksum and compatibility check → `WRITE` → `VERIFY`.

**IC content checksums** (CRC-32 over `u16 LE` words at addresses `0 … code_words−1` in
ascending order, bits above `code_bits` zeroed, **reserved areas and `reserved_first_word`
are skipped**):

- **exact** (`image_crc_raw`) — the words as they are;
- **normalized** (`image_crc_norm`) — before computing, in each recognized calibration trace
  (research R9) the immediate value of `MOV A,#k` is replaced by 0, and in each serial-number stub
  (8 consecutive `RET #imm`) `imm` is replaced by 0. It is the same for all ICs programmed with the same
  firmware, regardless of individual calibration and serial number.
- `ACTIVE_PACKAGE`: the expected `image_crc_norm` is computed from the effective image of the active package in the
  form it will have in the IC after `PROGRAM` (unset words — all ones, or NOP with
  `SECURE_FILL`; the fuse; calibration stubs — in the trace form with an arbitrary value; serial numbers — any),
  to which **the same** normalizer is applied as to the IC contents — so the sums match
  bit for bit. No active package → `STORE_EMPTY`; a different IC in the package → `CHIP_ID_MISMATCH`.
- **Special cases of `ACTIVE_PACKAGE`** (all are a `PATCH_IMAGE_MISMATCH` refusal with nothing written, so
  safe): an IC with unfinished calibration (a measurement loop after `CALIB_FAILED` or an interruption)
  — retry `PROGRAM` first; an IC programmed with another tool (for example without
  calibration) — use `RAW`/`NORMALIZED` with the sum from `patch --check` of a reference IC.

Rules:

- Content sum ≠ expected → `PATCH_IMAGE_MISMATCH`; **nothing is written**; `RESULT` contains
  both actual sums.
- For each word `(current AND value) == value` must hold, that is, only 1→0 transitions. The first
  violation → `PATCH_CONFLICT`, `RESULT.fail_addr/expected(=value)/actual(=current)`; **nothing is
  written**.
- Both checks are performed completely before the first write. `CHECK_ONLY` performs them and returns
  `RESULT` without writing (with `expect_mode = RAW/NORMALIZED` and `expected_crc32 = 0` it only returns the
  sums, without comparing).
- `VERIFY` rereads all user memory: the exact sum must equal the sum of the initial contents
  with the `value`s applied; otherwise `VERIFY_FAILED`.
- Words that already equal `value` are not written. The others are written in IC blocks, and the remaining words of a block are
  all ones (unchanged).
- `addr` in a reserved area → `PKG_RESERVED_AREA` with `fail_addr`; `addr` ≥ `code_words` or the same address
  twice → `BAD_PAYLOAD`.
- A fuse word that enables protection without `ALLOW_SECURITY_FUSE` → `PKG_SECURITY_NOT_ALLOWED`.
- Allowed for OTP and FLASH (for FLASH — without erasing). From a PC only; not started locally.
- `RESULT.data_words` = the number of words actually written.

Stage codes: `0x01 PKG_CHECK, 0x02 ID, 0x03 ERASE, 0x04 BLANK_CHECK, 0x05 WRITE, 0x06 VERIFY,
0x07 CALIBRATE, 0x08 FUSE, 0x09 READ, 0x0A STORE, 0x0B RUN, 0x0C MEASURE, 0x0D RESTORE, 0x0E SELF_TEST, 0x0F FINAL_VERIFY`.

`OP_CANCEL` safely stops the operation (the voltages are removed) and ends it with `CANCELLED`; for `COPY` in
`MEASURE` the reference is restored first. For `RUN`, `OP_CANCEL` is the normal stop.

**A dropped session (DTR ↓, USB reset/suspend) does not interrupt an operation (FR-023)**: all the data is already in the programmer,
the operation runs to the end, and the result is available through `OP_RESULT(0)` after reconnecting; events during
the client's absence are not buffered. The exception is `RUN`: it stops with a safe removal of the voltages and the
code `HOST_DISCONNECTED`.

**`RESULT`**:

| Type | Field |
|---|---|
| `u16` | `op_id` |
| `u8` | `op_type` |
| `u8` | `source`: 0 — client, 1 — local start (a control, a signal from a handler machine, or autostart at power-up) |
| `u16` | `status` (code from §6) |
| `u8` | `stage` — the stage of completion / error |
| `u16` | `chip_id_found` (`0` — not found) |
| `u8` | `family_found` |
| `u16` | `fail_addr` (`0xFFFF` — n/a) |
| `u16`, `u16` | `expected`, `actual` (for `VERIFY`/`BLANK_CHECK`) |
| `u8` | `serial_valid`; `u64 serial_written` |
| `u16` | `fuse_written` (`0xFFFF` — not written) |
| `u32` | `data_words`, `u32 data_crc32` (for `READ`/`COPY`) |
| `u32` | `elapsed_ms` |
| `u8` | `calib_count`, then `calib_count` × `{u8 kind (1 IHRC, 2 ILRC, 3 BG), u8 value, u32 target_hz, u32 measured_hz, u8 mode, u8 master_value, u8 site_status}` (`site_status`: 0 OK, 1 FAILED — not finished, 2 FORCED — finished with the best value, 4 COPIED — the reference value, 5 MASTER_LOOP — a measurement loop in the reference; 3 — reserved) — for `PROGRAM`: `value` = the calibration result of the copy; for `COPY`: `master_value` = the reference's `k`, `target_hz` = the determined target, `mode` = the mode actually applied |
| `u32, u32` | `image_crc_raw`, `image_crc_norm` — IC content sums at the end of the operation (`READ`, `COPY`, `VERIFY`, `PROGRAM`, `PATCH`; for `PATCH` without a write — the current contents); 0 — not computed |
| `u8` | `warnings`: bit0 `CALIB_VALUE_COPIED` (no target determined, `k` carried over), bit1 `CALIB_SNAPPED` (the target was rounded to a standard one), bit2 `MASTER_MODIFIED` (the reference was temporarily modified and restored), bit3 `RESUMED` (an OTP programming retry), bit4 `CALIB_FORCED` (a stub was finished with the best value after a failure), bit5 `SERIAL_DUPLICATED` (`COPY`: the reference's serial number was not processed and will be duplicated), bit6 `CALIB_UNRECOGNIZED` (`COPY`: the dump has `MOV A,#k` immediately before `MOV IO(reg),A`, where `reg` is the IC's `IHRCR`/`ILRCR`/`BGTR`, outside recognized traces; the address of the first such place is in `fail_addr`; the copies get the reference's value, FR-053) |
| `u8` | `v_rail` (0 VDD, 1 VPP, `0xFF` — n/a), `u16 v_set_mV`, `u16 v_meas_mV` — for `VDD_FAULT`/`VPP_FAULT`: the rail, the set voltage, and the last measured voltage |
| `u8` | `level_count`, then `level_count` × `{u8 rail, u16 set_mV, u16 meas_mV, u16 settle_us, u8 ok}` — for `SELF_TEST` |

`RESULT_READ` reads the result words of the last `READ` (`2 × data_words` bytes; the client checks
`data_crc32`). The data is valid until the next `PKG_BEGIN`, `READ`, or `COPY`.

## 5. Events

| Code | Name | Payload |
|---|---|---|
| `0x8001` | `EVT_PROGRESS` | `u16 op_id, u8 op_type, u8 stage, u8 stage_index, u8 stage_count, u8 percent_total` |
| `0x8002` | `EVT_OP_DONE` | `RESULT` |
| `0x8003` | `EVT_OP_STARTED` | `u16 op_id, u8 op_type, u8 source` |
| `0x8004` | `EVT_UART_RX` | `u8 data[]` |
| `0x8005` | `EVT_UART_LINK` | `u32 baud` |
| `0x8006` | `EVT_PKG_STATE` | `u8 pkg_state, u32 package_crc32` (after a commit, a delete, or detection of corruption) |

Events are sent only in an established session (after a successful `HELLO`). `EVT_PROGRESS` is sent when the
stage changes and at least once every 500 ms. Events are not acknowledged;
a gap is noticeable from a break in the event `seq`.

## 6. Status codes

| Code | Name | When |
|---|---|---|
| `0x0000` | `OK` | |
| `0x0101` | `FRAME_ERROR` | in `NAK`: COBS/length/CRC |
| `0x0102` | `UNKNOWN_COMMAND` | |
| `0x0103` | `BAD_PAYLOAD` | wrong length/field values |
| `0x0104` | `PROTO_VERSION` | incompatible major version |
| `0x0105` | `NO_SESSION` | a command before `HELLO` |
| `0x0106` | `BUSY` | an operation / upload is in progress |
| `0x0107` | `NOT_ALLOWED_NOW` | e.g. `PKG_DATA` without `PKG_BEGIN`, `UART_TX` without `RUN` |
| `0x0108` | `INTERNAL` | |
| `0x0201` | `PKG_CHIP_UNSUPPORTED` | |
| `0x0202` | `PKG_TOO_LARGE` | |
| `0x0203` | `PKG_HEADER_INVALID` | |
| `0x0204` | `PKG_OFFSET` | |
| `0x0205` | `PKG_CRC_MISMATCH` | |
| `0x0206` | `PKG_RESERVED_AREA` | |
| `0x0207` | `PKG_FUSE_CONFLICT` | |
| `0x0208` | `PKG_SECURITY_NOT_ALLOWED` | |
| `0x0209` | `PKG_CALIB_UNKNOWN` | |
| `0x020A` | — | reserved |
| `0x020B` | `PKG_CHIP_READ_ONLY` | |
| `0x020C` | `PKG_OTP_BLANK_CHECK_REQUIRED` | `SKIP_BLANK_CHECK` for OTP in the package or in `op_flags` |
| `0x0301` | `STORE_EMPTY` | no active package |
| `0x0302` | `STORE_CORRUPTED` | the CRC of the stored package does not match |
| `0x0303` | `STORE_WRITE_FAILED` | flash programming error |
| `0x0304` | — | reserved |
| `0x0401` | `CHIP_NOT_FOUND` | |
| `0x0402` | `CHIP_ID_MISMATCH` | `RESULT.chip_id_found` contains the actual ID |
| `0x0403` | `CHIP_NOT_BLANK` | |
| `0x0404` | `VERIFY_FAILED` | |
| `0x0405` | `CALIB_FAILED` | frequency outside the ±10 % tolerance or no signal; after a measurement loop nothing is written (except `CALIB_FINALIZE_ON_FAIL`) |
| `0x0406` | `CHIP_PROTECTED` | |
| `0x0407` | `VDD_FAULT` | |
| `0x0408` | `VPP_FAULT` | |
| `0x0409` | `OP_UNSUPPORTED` | e.g. `ERASE` for OTP |
| `0x040A` | `CANCELLED` | |
| `0x040B` | `HOST_DISCONNECTED` | `RUN` only: stopped because of a dropped session |
| `0x040C` | `MASTER_NOT_REWRITABLE` | `MEASURE_MASTER` for an OTP reference |
| `0x040D` | `MASTER_RESTORE_PENDING` | a reference restore is unfinished |
| `0x040E` | `MASTER_MODIFY_NOT_ALLOWED` | `MEASURE_MASTER` without `ALLOW_MODIFY_MASTER` |
| `0x040F` | `PATCH_CONFLICT` | appending requires a 0→1 transition |
| `0x0410` | `PATCH_IMAGE_MISMATCH` | the IC content sum does not equal the expected one |
| `0x0411` | `SELFTEST_CHIP_PRESENT` | self-test: an IC is connected |
| `0x0412` | `SELFTEST_FAILED` | self-test: a voltage level was not reached (details in `RESULT.levels[]`) |
| `0x0413` | `CHIP_ALREADY_PROGRAMMED` | OTP reprogramming: the contents already equal the effective image, the calibrations are finished |
| `0x0414` | `COPY_MASTER_UNCALIBRATED` | `COPY`: a measurement loop was found in the reference; nothing was written to storage |
| `0x0415` | `CALIB_WRONG_SITE` | the IC is running a different calibration stub than the one the programmer is calibrating: the first frequency is outside 1/4…4 of the expected one, there is no response to a step, or (BG) PA0 is not driven (FR-055); nothing is written, `CALIB_FINALIZE_ON_FAIL` has no effect |

### 6.1. Setting voltages (FR-040)

After every change of VDD or VPP, communication with the IC continues only when (a) the stage's minimum
pause has elapsed (100 µs after VPP on entering the mode, 500 µs after VDD, 1 ms when switching to
read voltages, 10 ms for write/erase) **and** (b) 3 consecutive measurements differ from the set value by no more
than ±2 % or ±30 mV (whichever is larger). If not reached within 20 ms → safe state, `VDD_FAULT`/`VPP_FAULT`,
`RESULT.v_rail/v_set_mV/v_meas_mV`. The same rule applies to 0 V when the voltages are removed. The set and measured
voltages are recomputed with the stored supply calibration (§4, "Supply calibration").

## 7. Mapping to `easypdkprog` commands (FR-030)

| `easypdkprog` | Sequence |
|---|---|
| `list` | `LIST_CHIPS` |
| `probe` | `OP_START(PROBE)` → `EVT_OP_DONE` |
| `-n X read f` | `OP_START(READ, id(X))` → `RESULT_READ` × k → file |
| `-n X write f [--fuse --allowsecfuse --securefill --noerase --noblankchk --noverify] [--name NAME] [--label TEXT]`; `--nocalibrate` is rejected by the client with the explanation "remove the calibration macro from the firmware"; `--serial` — "the serial number is generated automatically (random)" | build the package with the flags (the default name is the file name) → `PKG_BEGIN/DATA/COMMIT` → `OP_START(PROGRAM)` |
| `-n X erase` | `OP_START(ERASE, id(X))` |
| `start [-r V]` | `OP_START(RUN, param=V)` → `EVT_UART_*` / `UART_TX` → `OP_CANCEL` |
| *(extension)* `copy [--ihrc keep\|target\|snap\|measure] [--ilrc keep\|target\|measure] [--bg keep\|restore] [--ihrc-freq HZ] [--ilrc-freq HZ] [--calib-mv MV] [--allow-modify-master]` | `OP_START(COPY, ext)` |
| *(extension)* `restore-master` | `OP_START(RESTORE_MASTER)` |
| *(extension)* `selftest` | the confirmation "the socket is empty" → `OP_START(SELF_TEST)` |
| *(extension)* `-n X patch ADDR=VALUE … (--expect-crc CRC [--normalized] \| --expect-package) [--check] [--allowsecfuse]` | `OP_START(PATCH, id(X), ext)`; `-n X patch --check` without an expected sum only shows the IC sums |
| `--noblankchk` for OTP | the client refuses before sending; the programmer — `PKG_OTP_BLANK_CHECK_REQUIRED` |

## 8. Example exchange

```
C→P  REQ  seq=1  HELLO       01 00 "padauk-cli 0.1"
P→C  RSP  seq=1  OK          INFO
C→P  REQ  seq=2  PKG_BEGIN   <header 128 B>
P→C  RSP  seq=2  OK          package_size=6660
C→P  REQ  seq=3  PKG_DATA    offset=0, 500 B          (frame corrupted on the wire)
P→C  NAK                     FRAME_ERROR
C→P  REQ  seq=3  PKG_DATA    offset=0, 500 B          (retry)
P→C  RSP  seq=3  OK          received=500
 …
C→P  REQ  seq=17 PKG_COMMIT
P→C  RSP  seq=17 OK          crc=0x1234ABCD calib=1 serial=0 words=2047
C→P  REQ  seq=18 OP_START    PROGRAM
P→C  RSP  seq=18 OK          op_id=7
P→C  EVT  #1     EVT_OP_STARTED op=7 PROGRAM source=host
P→C  EVT  #2     EVT_PROGRESS   op=7 ERASE 3/8 20%
 …
P→C  EVT  #9     EVT_OP_DONE    RESULT{status=OK, calib: IHRC 0x5A 8000000→7998200 Hz}
```
