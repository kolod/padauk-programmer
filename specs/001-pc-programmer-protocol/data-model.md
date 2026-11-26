# Data Model: PC ⇄ programmer exchange protocol

The byte formats on the wire are in [contracts/protocol.md](contracts/protocol.md) and
[contracts/package-format.md](contracts/package-format.md). Here are the entities, their relations, rules, and states.

## Entities

### Frame
`type`, `flags`, `seq`, `code`, `status`, `len`, `payload`, `crc32`.
- Valid ⇔ COBS decoded, `len` = actual length − 14, `len ≤ max_payload`, the CRC matches.
- Invalid → `NAK`, not executed.

### Session (the port session)
| Field | Description |
|---|---|
| `open` | DTR is high and USB is configured |
| `established` | there was a successful `HELLO` in this session |
| `client_proto` | the client version from `HELLO` |
| `last_req_seq`, `last_rsp` | the cache for retries (FR-009) |
| `evt_seq` | the event counter |

A change of `open` → resets `established` and the cache and cancels an unfinished upload. The current IC operation is **not** interrupted (FR-023), except `RUN` (`HOST_DISCONNECTED`).

### Package (the firmware package)
Header + Words[N] + DefinedMask + `package_crc32` (see the contract).
Invariants after commit: `package_crc32` is valid; all `PKG_COMMIT` checks passed; it refers to
exactly one `ChipDescriptor`.

### Upload
| Field | Description |
|---|---|
| `slot` | the inactive slot |
| `expected_size` | `package_size` from the `PKG_BEGIN` header |
| `received` | bytes received |
| `header_copy` | the header from `PKG_BEGIN` |
| `erased_pages` | the slot pages already erased |

States:

```
IDLE ──PKG_BEGIN ok──► RECEIVING ──PKG_DATA (received == expected)──► COMPLETE ──PKG_COMMIT ok──► IDLE (the new package is active)
  ▲                       │  │                                           │
  └──── PKG_ABORT / error / dropped session / PKG_COMMIT error ─────────┴──► IDLE (the active package is unchanged)
```

### StorageSlot (a flash slot)
An area of fixed size (F072: 8 KB, 4 pages). The contents are interpreted **only** if the active `StateRecord`
points to the slot. An inactive slot is an upload buffer or a `READ` result.

### StateRecord (a state log record, 40 bytes)
| Type | Field | Description |
|---|---|---|
| `u32` | `magic` | `"PDKS"` |
| `u32` | `rec_seq` | the monotonic record number |
| `u8` | `active_slot` | 0, 1, or `0xFF` — no package |
| `u8` | `cfg_copy_ihrc` | the IHRC mode for the local `COPY` (0 or 2), default 0 |
| `u16` | `flags` | bit0 `MASTER_RESTORE_PENDING` — the active package is a reference dump that was modified and not restored; bit15 — `supply_cal` is valid |
| `u32` | `package_size` | |
| `u32` | `package_crc32` | |
| `i8[4]` | `supply_cal` | supply calibration in parts per thousand: VDD measurement, VPP measurement, VDD output, VPP output (contracts/protocol.md, `CAL_*`); without bit15 in `flags` — `0xFFFFFFFF` (the former serial-number counter), records of older versions are read as "not calibrated" |
| `u32` | `ok_count` | |
| `u32` | `fail_count` | |
| `u8` | `cfg_copy_ilrc` | the ILRC mode for the local `COPY` (0), default 0 |
| `u8` | `cfg_copy_bg` | 1 — restore BandGap on a local `COPY` (the default), 0 — carry over |
| `u16` | `reserved` | `0xFFFF`; pads the record to 40 B (a multiple of 8 — for MCUs that program flash in double words) |
| `u32` | `rec_crc32` | CRC of bytes 0–35 (the whole record is 40 B, 51 records per 2 KB page) |

- The current record is the valid one (magic + CRC) with the largest `rec_seq` of the two log pages.
- A new record is appended: after a commit (a new `active_slot`), after each
  finished IC write (`ok/fail`), after `PKG_DELETE`, `STATS_RESET`,
  `CONFIG_SET`, and on setting and clearing `MASTER_RESTORE_PENDING`.
- While `MASTER_RESTORE_PENDING = 1`, `PKG_BEGIN`, `PKG_DELETE`, and `COPY` are rejected
  (`MASTER_RESTORE_PENDING`) so as not to lose the dump; `RESTORE_MASTER`, `PKG_READ`, and `PROBE` are allowed.
- A page is full → erase the other page, write a copy of the current record there, and keep writing there.

### PackageState (computed at start and before each `PROGRAM`/`VERIFY`)
| Value | Condition |
|---|---|
| `EMPTY` | there is no valid `StateRecord` or `active_slot = 0xFF` |
| `VALID` | the package CRC in the slot = the record's `package_crc32` and = the CRC in the package trailer |
| `CORRUPTED` | otherwise; standalone and any programming is blocked (`STORE_CORRUPTED`) |

### ChipDescriptor (the table in the firmware)
The public part (`CHIP_DESC`): ID, family, size, word width, RAM, available operations, reserved
areas, names. The **internal** part — voltages, write block parameters, the number of erase pulses,
the calibration opcode tables — is not passed to the client (principle I).

### EffectiveImagePlan (RAM, for the duration of `PROGRAM`/`VERIFY`)
| Field | Description |
|---|---|
| `fuse` | the fuse value or "do not write" |
| `calib_sites[≤16]` | `{addr, kind, target_hz, mV, regs}` |
| `serial_sites[≤4]` | addresses of the serial-number stubs |
| `fill_nop_until` | the `SECURE_FILL` boundary |

The word for address `a` = a function(package word/mask, plan, calibration phase, current serial number).
It is built by the same code as the `PKG_COMMIT` validation.

### Operation
| Field | Description |
|---|---|
| `op_id` | u16, increasing |
| `op_type`, `source` | |
| `stage`, `stage_index`, `stage_count`, `percent` | for `EVT_PROGRESS` |
| `cancel_requested` | |
| `result` | the `RESULT` after completion |

```
IDLE ──OP_START / local_start──► RUNNING ──engine steps──► DONE(status)
                               │
                               ├─ OP_CANCEL ─────────► DONE(CANCELLED)
                               └─ dropped session ───► continues (only RUN → DONE(HOST_DISCONNECTED))
```
- Any transition into DONE MUST go through the engine's "safe state": VDD = VPP = 0, the IC lines Hi-Z.
- A dropped session does not cancel any operation except `RUN`; the result is kept for `OP_RESULT(0)`.
- For `COPY` in the `MEASURE` stage, cancellation means "restore the reference and finish"
  (see `CopyJob`), not an immediate stop.
- A local start is turned into an operation by priority (research R16): the self-test did not
  pass → `SELF_TEST`; `MASTER_RESTORE_PENDING = 1` → `RESTORE_MASTER`; otherwise the requested operation
  (`PROGRAM` or `COPY` with the `cfg_copy_*` modes).
- Only one operation at a time; during `RECEIVING` operations are forbidden (`BUSY`).
- After `DONE` for `PROGRAM`: a `StateRecord` is written (ok/fail); `CHIP_ALREADY_PROGRAMMED` does not change the counters.

### ProgramJob (the `PROGRAM` operation)

```
PKG_CHECK → ID → ERASE (FLASH) → BLANK_CHECK ─┬─ blank ─────────────► WRITE* → VERIFY → CALIBRATE(each stub) → PKG_CHECK → FUSE → FINAL_VERIFY → DONE(OK)
                                              ├─ OTP, compatible ───► WRITE*(the rest) → VERIFY → CALIBRATE(unfinished) → PKG_CHECK → FUSE → FINAL_VERIFY → DONE(OK, RESUMED)
                                              ├─ OTP, all done ─────► DONE(CHIP_ALREADY_PROGRAMMED)
                                              └─ incompatible ──────► DONE(CHIP_NOT_BLANK, addr)
CALIBRATE failed ─► (CALIB_FINALIZE_ON_FAIL and source = 0 ? finish with the best k, then as OK, warnings.CALIB_FORCED
                                                            : stop, write nothing) → DONE(CALIB_FAILED)
```

`WRITE*` — during the write the package CRC is computed on the fly from the data being written (FR-049). `VERIFY` —
taking stubs into account (a calibration stub = a measurement loop); `FINAL_VERIFY` — an exact comparison
of all memory with the image into which the actual `k`, the serial number, and the fuse have been substituted (FR-048); from it
also `RESULT.image_crc_raw/norm`.

Word compatibility for OTP: `(content AND target) == target`; for the words of a calibration stub the target may be
any of three states (blank, a measurement loop, the finished form with an arbitrary `k`); a serial-number
stub — blank (a new random number will be written) or already written (the number is taken from the IC). The check does not
use any programmer state, so a retry is possible later and with a different programmer.

The serial number is a random 64-bit number generated for each IC before the `WRITE` stage
(research R18); there is no state between ICs.

### CalibFootprint (a calibration trace in a dump)
| Field | Description |
|---|---|
| `addr` | the position of the first word of the trace |
| `kind` | IHRC / ILRC / BG — by the register address from the `ChipDescriptor` |
| `master_value` | `k` from `MOV A,#k` |
| `target_hz`, `mV` | determined by the `COPY` mode (research R9) |

The `ChipDescriptor` (the internal part) contains the addresses of `IHRCR`, `ILRCR`, `BGTR`, `GPCC`, `GPCS` and the nominal
ILRC — needed for recognizing traces and restoring stubs.

### CopyJob (the `COPY` operation)

```
ID → READ → [a measurement loop in the dump?] ── yes, without ALLOW_MASTER_LOOP ──► DONE(COPY_MASTER_UNCALIBRATED), storage unchanged
           → [traces found?]
   ├─ no / KEEP_VALUE ───────────────────────────────────────────► STORE(a KEEP_VALUE package) → DONE
   ├─ TARGET_FROM_HOST / SNAP_STANDARD ──────────────────────────► STORE(a package with stubs) → DONE
   └─ MEASURE_MASTER (FLASH + ALLOW_MODIFY_MASTER):
        STORE(dump, KEEP_VALUE) → log: PENDING=1
        → MEASURE(trace 1) … MEASURE(trace n)      (for each trace in mode 3: erase + write the dump,
                                                     where only this trace is a loop and the rest as in the reference; measure;
                                                     no signal within 1 s → the reference's k, site_status = COPIED)
        → RESTORE (erase + write the dump + VERIFY) → log: PENDING=0
        → STORE(a package with stubs, target = f_master) → DONE
   Reference erases = n + 1 (n — the traces in mode 3; BG is not measured).
   A measurement error or OP_CANCEL during MEASURE → RESTORE first, then DONE(code);
   a dropped session interrupts nothing; a failed RESTORE or a power cut →
   PENDING stays → RESTORE_MASTER is needed
```

### ImageChecksum (IC content sums)
| Field | Description |
|---|---|
| `image_crc_raw` | CRC-32 of user memory as is (without reserved areas, bits above `code_bits` = 0) |
| `image_crc_norm` | the same after normalization: `k` in finished calibration traces and `imm` in serial-number stubs → 0. A serial-number stub in a dump is exactly 8 `RET #imm` in a row: longer runs are tables and are not normalized, so that a change to a table does not hide from `PATCH`; measurement loops are not normalized either |

It is computed on the fly while reading the IC (without a RAM copy) or from the package's effective image
with the same normalizer. It is used by `PATCH` (a mandatory check before and after writing) and
returned in the `RESULT` of operations that read the IC.

### PatchJob (the `PATCH` operation)
`ID → READ (all memory: the sums + the current values of the patch addresses) → sum check → 1→0 check →
WRITE (only the changed words) → VERIFY (all memory: raw = the expected raw)`. Any failed check
before `WRITE` → DONE with nothing written.

### SelfTestState (RAM, not stored)
| Field | Description |
|---|---|
| `state` | NOT_RUN / RUNNING / PASSED / FAILED / CHIP_PRESENT (`STATUS.selftest_state`) |
| `levels[]` | `{rail, set_mV, meas_mV, settle_us, ok}` of the last self-test |

```
start ─► SELF_TEST(source=1) ─┬─ PASSED ───────────► local programming allowed
                              ├─ FAILED ───────────► LOCAL_START_BLOCKED (local_start → SELF_TEST)
                              └─ CHIP_PRESENT ─────► LOCAL_START_BLOCKED (local_start → SELF_TEST)
SELF_TEST from a PC updates the same state.
```

### PowerFault (in `RESULT` on `VDD_FAULT`/`VPP_FAULT`)
`v_rail`, `v_set_mV`, `v_meas_mV` — the rail, the set and the last measured voltage (contracts §6.1).

### ReadResult
The raw words in the inactive slot + `data_words`, `data_crc32`, `chip_id12` in RAM. Destroyed by the next
`PKG_BEGIN`, `READ`, `COPY`.

## Relations

```
Session 1──0..1 Upload ──► StorageSlot (inactive)
StateRecord ──active_slot──► StorageSlot ──contains──► Package ──chip_id12──► ChipDescriptor
Operation(PROGRAM) ──uses──► Package + EffectiveImagePlan ──updates──► StateRecord
Operation(READ/COPY) ──writes──► StorageSlot (inactive) ──COPY: commit──► StateRecord
```
