# Research: PC ⇄ programmer exchange protocol

**Feature**: [spec.md](spec.md) · **Plan**: [plan.md](plan.md) · **Date**: 2026-10-09

Each decision: **Decision** — what was chosen, **Rationale** — why, **Alternatives** — what was rejected.

---

## R1. Framing the USB CDC byte stream

- **Decision**: a frame is encoded with **COBS** (Consistent Overhead Byte Stuffing), and the frame delimiter is the byte `0x00`.
- **Rationale**: CDC is a byte stream with no message boundaries. COBS guarantees that `0x00` appears only as a
  delimiter, so after any corruption the receiver resynchronizes on the next `0x00`. The overhead
  is deterministic (≤ 1 byte per 254), and the encoder and decoder take ~60 lines of C and are available in Python.
- **Alternatives**: SLIP — variable overhead (up to 2×), worse for binary images;
  a sync byte + length — after the length is corrupted, synchronization is lost until a timeout;
  HDLC bit-stuffing — excessive for USB.

## R2. Integrity checking of frames and the package

- **Decision**: **CRC-32/ISO-HDLC** (polynomial 0x04C11DB7, reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF —
  the same as `zlib.crc32`) for both frames and the firmware package. The package CRC is computed by the client over the whole
  package before sending; it is a separate, end-to-end check on top of the frame CRC.
- **Rationale**: for messages up to ~11 KB CRC-32 has a Hamming distance of 4 — it detects all 1-, 2-, and 3-bit
  errors and all burst errors up to 32 bits (FR-008). Frames (≤ ~0.5 KB) and the package (≤ 8 KB) fall within these limits.
  The STM32F072 has a hardware CRC block with a programmable polynomial and reversal of input/output bits;
  a software table variant (1 KB, or 64 B for a nibble table) serves other platforms and the host build.
  The Python client uses `zlib.crc32` with no dependencies.
- **Alternatives**: CRC-32C — a better HD on short messages, but it is not in the F0 hardware block in
  its "native" form and is absent from the Python standard library; SHA-256 for the package — protection against
  deliberate forgery is not needed (the threat is random errors), and it costs code and time on a Cortex-M0;
  CRC-16 — insufficient for 8 KB.

## R3. Delivery reliability

- **Decision**: **stop-and-wait** (window 1). The client has at most one outstanding request; every new
  request gets the next `seq` (u16). The programmer caches the last response: a request with the same `seq`
  gets the same response without being executed again (FR-009). A corrupted frame → `NAK` (without `seq`),
  and the client repeats the current request. The client's response timeout is 300 ms, with up to 5 retries.
- **Rationale**: USB FS gives ~1 MB/s; an 8 KB package is ~16 frames of 512 B, so even with a 2–3 ms delay
  per frame the upload takes < 0.1 s (SC-003 ≤ 3 s). A sliding window gives no gain but complicates the cache.
- **Alternatives**: a sliding window / selective repeat — needless complexity; relying on
  USB "not losing data" — contradicts FR-008 and does not protect against firmware/driver errors or a drop.

## R4. Long operations, progress, and cancellation

- **Decision**: `OP_START` answers `OK` at once with an `op_id` (≤ 100 ms, FR-024). After that the programmer sends
  `EVT_PROGRESS` events (on a stage change and at least once every 500 ms) and `EVT_OP_DONE` with the summary. Events are not
  acknowledged; the summary of the last operation can always be obtained through `OP_RESULT`, so losing
  an event is not a problem. The operation engine calls the protocol layer's `poll()` between atomic steps
  (a write block, a read word, an erase pulse), serving the commands allowed during an operation:
  `HELLO`, `GET_INFO`, `GET_STATUS`, `PKG_INFO`, `STATS_GET`, `CONFIG_GET`, `OP_RESULT`, `OP_CANCEL`,
  `UART_TX` (the "During an operation" column in contracts/protocol.md §4 is authoritative); the rest → `BUSY` (FR-021).
- **Rationale**: bit-bang with the IC is blocking and timing-sensitive, so the protocol cannot be
  handled at an arbitrary moment; the longest atomic step — the FLASH_2 erase pulse (40 ms) — is less
  than 100 ms, so the response requirement is met. No RTOS is needed.
- **Alternatives**: an RTOS with a separate protocol task — more RAM/flash, a risk of bit-bang jitter;
  handling commands in the USB interrupt — unsafe to change state during an operation.

## R5. Storage layout in flash and atomicity

- **Decision**: two identical **slots** for the package and a separate **state log** of two pages. The active
  slot, its generation and the package CRC, the settings, and the statistics are written as log records
  with their own CRC. An upload is written to the inactive slot; commit = a check of the package CRC in flash +
  appending a new log record. The layout and the number of slots are set by the platform (`GET_INFO`); for
  the primary MCU they are determined after it is chosen (the next specification). The F072 debug target (64 KB,
  2 KB page):

  | Area | Addresses | Size |
  |---|---|---|
  | Firmware | `0x0800_0000–0x0800_AFFF` | 44 KB |
  | Slot 0 | `0x0800_B000–0x0800_CFFF` | 8 KB |
  | Slot 1 | `0x0800_D000–0x0800_EFFF` | 8 KB |
  | State log (2 pages) | `0x0800_F000–0x0800_FFFF` | 4 KB |

- **Rationale**: a power cut before the log is written leaves the previous pointer — the old package stays
  active (FR-011, SC-007); an incompletely written log record has an invalid CRC and is ignored. The contents of
  an inactive slot are never interpreted as a package, so it can be used as a buffer for the
  IC read result (up to 8192 bytes of raw words — enough for the PMS133 with 4096 words) with no RAM buffer.
  Slots that do not contain the active package, and the spare log page, are erased **in the background**
  (`core/storage/housekeeping`, one page per pass of the main loop) right after `PKG_COMMIT`,
  `PKG_DELETE`, an interrupted upload, or a start with a "dirty" slot. An upload writes to an already clean
  slot and erases nothing inside the command; only if the background erase has not caught up yet, the page
  is finished on the first `PKG_DATA` access (≤ 1 page per command). On flashes that erase
  asynchronously (W25Q), the background erase is suspended (erase suspend) for reading and writing other pages;
  on the internal F072 flash it is synchronous, but happens in idle time. The platform sets the number of slots
  (2 … 4): with 3+ slots even back-to-back uploads always land in a clean slot. A side
  effect is that a deleted or replaced package is physically erased, not merely no longer active.
  The platform also sets the flash write unit (2 B on STM32F0, 8 B on STM32G4 with ECC, where each
  double word is written once): the core holds back bytes that do not fill a unit until the next
  `PKG_DATA`, and pads the last unit with `0xFF` before `PKG_COMMIT`. The `READ` result
  buffer in a free slot MUST be reserved from the background erase the same way as the slot of the
  current upload (`slots_set_upload`). No overwrite-without-erase (0x0000 over
  data) is needed, which not all MCUs support — better for portability (principle IV).
- **Alternatives**: one slot + an 8 KB RAM buffer — not atomic on a drop during a flash write; a header
  with a "generation" in the slot itself — raw read data may accidentally look like a header, and
  corruption of the new slot "resurrects" the old package; an external EEPROM/SPI flash — an extra part,
  possible on other platforms through the HAL.
- **Risk / decision**: the scope of the feature does not fit confidently into the F072's 44 KB, so the primary platform will be
  a more powerful STM32 (the choice is the next specification). On the F072 as a debug target some features
  (for example, `MEASURE_MASTER`, `PATCH`) MAY be disabled by build configuration; `INFO.capabilities` reflects
  this.

## R6. Flash wear from the log

- **Decision**: a log record is **40 bytes** (36 bytes of fields + CRC-32, data-model `StateRecord`); every
  successful/failed IC write appends one record; when a page is full, the next record goes
  to the second page. The second (spare) page is erased in the background as soon as the current record is on the current
  page, so a page switch erases nothing inside a command; the page with the current record
  (for example, if the first record on a new page failed) is never erased.
- **Rationale**: 51 records per page × 10,000 erase cycles ≈ 510,000 ICs per page —
  more than the lifetime of the socket. A drop during the erase of the second page loses no data: the full page
  remains. The serial-number counter survives power-off (US3).
- **Alternatives**: writing only when the package changes + a RAM counter — serial numbers repeat after
  power-off; ST EEPROM emulation — tied to an ST library.

## R7. The firmware package format

- **Decision**: a binary little-endian format: a fixed 128 B header with its own CRC, then the image words
  (`u16 × N`), a bit mask of defined words (a multiple of 16 bits), and a CRC-32 of the whole package. Details are in
  [contracts/package-format.md](contracts/package-format.md).
- **Rationale**: the header is checked in `PKG_BEGIN` before any data is received (an unsupported IC,
  an oversized image, a forbidden protection fuse) — the error is visible at once. The mask distinguishes "not defined" from
  any value, so sentinels like 0xFFFF are not needed (FR-004). The maximum package for
  ICs with writing (PFS173, 3072 words) = 128 + 6144 + 384 + 4 = 6660 B < the 8 KB slot.
  **The limits differ for reading and writing**: `max_package_size` (from `INFO`) limits the write package — on F072
  8192 B, that is, up to ~4000 words; `max_read_words` limits reading — 4096 words (the raw data takes exactly
  a slot). A 4096-word package (8836 B) is not needed: an IC with 4096 words (PMS133) is read-only.
- **Alternatives**: sending Intel HEX and parsing it in the programmer — contradicts principle I (file formats
  live on the PC) and wastes flash; TLV metadata — more flexible, but validation is more complex; a format version
  in the header provides extensibility.

## R8. Who knows about the fuse, calibration, and serial number

- **Decision**: the programmer determines everything (principle I):
  - **fuse**: if the image defines the last program memory word, it is treated as the fuse; if the header
    also has `FUSE_PRESENT` with a different value — the error `PKG_FUSE_CONFLICT`;
  - **calibration**: the programmer looks for free-pdk stubs for the IC's core width (see below),
    rejects a package with an unknown stub, and performs the calibration itself;
  - **serial number**: a search for 8 instructions `RET 'F','P','S','E','R','I','A','L'`; each stub found
    gets a random 64-bit number (R18), and there are no parameters in the package.
  During programming the image is read from flash and transformed "on the fly" (calibration stubs, serial number,
  NOP fill, fuse exclusion) using a small table of positions computed when the package is validated.
  **Searching for calibration stubs.** No one passes the addresses: at `PKG_COMMIT` the programmer walks all
  program memory and looks for the pattern — a sequence of `AND A,#imm` (the opcode by core width: `0x14xx` —
  13 bit, `0x2Cxx` — 14, `0x54xx` — 15):
  - IHRC/ILRC, 10 words: `'R'`, `'C'`, type (1/2), frequency (4 bytes), voltage (2 bytes), register address;
  - BandGap, 14 words: `'B'`, `'G'`, 3, voltage (2 bytes), `BGTR`, `GPCC`, `GPCS`, `0` × 6;
  - ≥ 6 consecutive `AND A,#…` that do not match a known pattern → `PKG_CALIB_UNKNOWN` with the address
    (most likely a stub from a newer SDK — silently writing it as code is dangerous);
  - up to 16 stubs; the positions found, the type, frequency, voltage, and registers are stored in the effective
    image plan. The single `GOTO` of the measurement loop is fixed up for the position found, so the stub works
    anywhere. Serial-number stubs and (during `COPY`) calibration traces are found the same way.
  A false match with ordinary code is practically impossible (10 `AND A` in a row starting with `'R','C'` make no sense);
  the reachability of a stub at start is not checked statically — its absence shows up as "no signal"
  during calibration (`CALIB_FAILED`).
- **Rationale**: the client becomes thin, and adding a new IC requires no client changes (SC-006).
  On-the-fly transformation needs no RAM copy of the image; the same stub detector is used by
  validation, the effective image, verification taking stubs into account (FR-047), the final check (FR-048),
  the retry (FR-044), and the normalized sum.
- **Alternatives**: the client patches the image (like `easypdkprog`) — violates principle I.

## R9. Copying and calibrated ICs

- **Decision**: `COPY` recognizes **calibration traces** in the dump and restores free-pdk calibration
  stubs in the package so that each copy is calibrated at `PROGRAM`. The mode is set **separately for each
  type** (`mode_ihrc`, `mode_ilrc`, `bg_restore` in the `ext` of the `COPY` command; FR-042):

  | Mode (IHRC / ILRC) | Target | IHRC | ILRC | When possible |
  |---|---|---|---|---|
  | `KEEP_VALUE` (0) | the stub is not restored, the reference's `k` | ✓ | ✓ | always |
  | `TARGET_FROM_HOST` (1) | the target from the client (`target_ihrc_hz` / `target_ilrc_hz`) | ✓ | ✓ | from a PC only |
  | `SNAP_STANDARD` (2) | at `PROGRAM` of each copy: measure the copy at the reference's `k` → the nearest SYSCLK from {16 M, 8 M, 4 M, 2 M, 1 M, 500 k} if the deviation is ≤ 10 %; otherwise the reference's `k` with a warning | ✓ | ✗ (the ILRC spread between samples is too large) | always |
  | `MEASURE_MASTER` (3) | the reference's actual frequency at its `k` | ✓ | ✓ | a FLASH reference + `ALLOW_MODIFY_MASTER`, from a PC only |

  BandGap: `bg_restore = 1` — a stub with a fixed target (3950 mV, as in the free-pdk macro), `0` — the reference's
  `k`. The addresses of `BGTR`/`GPCC`/`GPCS` and `IHRCR`/`ILRCR` come from the IC table. For local copying
  (`CONFIG_SET`) IHRC 0 or 2, ILRC 0, and BG 0/1 are allowed; the default is `0, 0, 1` (= the former
  `KEEP_RC_RESTORE_BG`). `COPY` of an IC with no write operation in the table (e.g. PMS133) →
  `OP_UNSUPPORTED`: the package created cannot be written.

  **Calibration in a foreign format** (FR-053): writing a constant to a trim register outside recognized
  traces (`MOV A,#k` → `MOV IO(IHRCR|ILRCR|BGTR),A`) — the code is copied as is, `warnings.CALIB_UNRECOGNIZED`
  with the address. Loading the factory value (`EASY_PDK_USE_FACTORY_*`) reads it from memory rather than a
  constant, so it does not raise the warning.

  **A measurement loop in the reference** (FR-051): the reference is not calibrated and does not work; there is no
  target in the loop. Locally — a refusal with nothing written to storage; from a PC — the client asks the user and, if they agree, repeats
  `COPY` with `ALLOW_MASTER_LOOP` and the target `TARGET_FROM_HOST`.

  **Several traces in `MEASURE_MASTER`.** The measurement loop is endless, so one trace is measured per rewrite
  of the reference; the traces before it run with the reference's values — the same conditions as
  the initial free-pdk calibration (sequentially by address). Reference erases = n + 1, where n is the
  number of traces in mode 3. A trace with no signal on PA3 for 1 s (the program never reaches it,
  PA3/PA4 are loaded by the board) → the reference's `k`, `site_status = COPIED`, a warning; the remaining traces go on.
  *A possible improvement (not in this feature):* a "loop with an exit" — a pulse on PA4 takes the CPU out of the loop before
  the trace continues, so all traces are measured in one rewrite (always 2 erases). It needs a new
  loop with an exactly known number of clocks (an instruction skip on PADAUK takes 2 clocks) and verification on a model and
  hardware; worthwhile if references with several RC calibrations turn out to be common.

  **Calibration traces** (positions relative to the start; the rest is `NOP`):

  | Type | Length | Nonzero words |
  |---|---|---|
  | IHRC / ILRC | 10 | [2] `MOV A,#k`, [5] `MOV IO(IHRCR\|ILRCR),A` |
  | BandGap | 14 | [7] `MOV A,#k`, [9] `MOV IO(BGTR),A` |

  A match requires the register address to equal the address from the IC table. A random match in
  ordinary code is unlikely and almost harmless: only a constant written to the same trim register
  changes.

  **The `MEASURE_MASTER` sequence**:
  1. `READ` the reference → the dump into the inactive slot → a `KEEP_VALUE` package → **commit** (the dump is safe);
  2. write a log record with the `MASTER_RESTORE_PENDING` flag and the dump CRC;
  3. for each IHRC/ILRC trace in turn: erase the reference; write the dump in which this trace is replaced
     by a measurement loop with the initial value `k` (`MOV A,#(k−1)`, because the loop first executes `ADD A,1`),
     and the other traces are unchanged; apply VDD (`mV` from the client or 5000) and measure the frequency on PA3 **without
     pulses on PA4** → `f_master(k)`;
  4. erase the reference, write the dump (`PROGRAM` of the `KEEP_VALUE` package with an erase), **verify**;
  5. write a log record without `MASTER_RESTORE_PENDING`;
  6. build the final package with the stubs restored (target = `f_master(k)`) → commit.

  A drop between steps 2 and 5: after power-up `STATUS.flags.MASTER_RESTORE_PENDING = 1`; the
  `RESTORE_MASTER` operation (from a PC or a local start) performs step 4 from the active dump package. If a blank IC of the
  same type is inserted instead of the reference, `RESTORE_MASTER` simply writes the copy with the reference's `k` into it —
  this is safe.

  `SNAP_STANDARD` needs `k` during `PROGRAM`; the programmer encodes it in the restored stub
  with the internal frequency value `0xFF0000kk` (impossible for a real stub, see package-format).

- **Rationale**: `f_master(k)` is the frequency at which the reference actually runs, that is, exactly what the copies must
  reproduce; the error is the trim register step (fractions of a percent) plus the difference in the measurement voltage.
  The transition "NOP → measurement loop" requires 0→1, so without erasing the reference a measurement
  is impossible; for OTP references modes 0–2 remain. Saving the dump as a committed package before the first
  erase guarantees that the reference can always be restored (SC-010).
- **Alternatives**: only carrying over `k` (the previous revision) — the copies have a frequency spread between
  samples; measuring a copy at the reference's `k` as the target — reproduces the copy's spread, not the reference's;
  measuring the reference without rewriting — a calibrated code has no frequency output on a pin.
- **Risk**: the PADAUK FLASH erase endurance (to be confirmed from the PFS154 datasheet) — `MEASURE_MASTER` adds
  (number of traces + 1) reference erase cycles; therefore only with explicit permission.

## R10. Detecting a protected IC

- **Decision**: after reading, the programmer checks the protection bit in the fuse word read; if protection
  is enabled — `CHIP_PROTECTED`, the result is not returned and no package is created.
- **Open risk**: how exactly PADAUK returns the data of a protected IC (zeros, garbage, or a correct fuse) is not described in
  the free-pdk code. **Verification on a real protected PFS154 is needed** (a hardware validation task).
  A fallback criterion — the image consists of identical words.

## R11. USB stack and identifiers

- **Decision**: **TinyUSB** (MIT) for CDC ACM. VID/PID — from the **pid.codes** program (VID `0x1209`, its own
  PID for open hardware); until obtained — the test PID `0x1209:0x0001`.
- **Rationale**: TinyUSB supports STM32F0/F4/G4, RP2040, and others — changing the MCU does not change the USB code (principle IV);
  MIT is compatible with GPL-3.0. Using ST's VID (`0x0483:0x5740`) in your own product is not appropriate.
- **Alternatives**: the ST USB Device Library — tied to ST and needs an extra permission with the GPL (as in
  the original); libopencm3 — LGPL, narrower MCU support.

## R12. The client's language and testing

- **Decision**: the reference client is **Python 3.8+** (Windows 7) with `pyserial`: a protocol library, building a package from
  Intel HEX, a CLI in the style of `easypdkprog` (FR-030). Tests — `pytest`.
- **Rationale**: cross-platform with no custom drivers, `zlib.crc32` and `struct` out of the box, quick
  to write and change. The client's performance requirements are minimal.
- **Alternatives**: C — duplicates the core, slower CLI development; Rust — good, but a higher barrier for contributors.

## R13. Verification without hardware

- **Decision**: the firmware core is built for the PC together with a fake HAL into a **programmer simulator** —
  an executable that speaks the protocol through stdin/stdout or TCP and emulates the flash (with the ability to
  "cut the power" at an arbitrary write operation) and the IC (a PFS154 model in memory). The client and
  contract tests run against the simulator; fault injection into frames is in the test's intermediate layer.
- **Rationale**: it allows SC-001, SC-002, SC-007 to be checked automatically thousands of times (principle VI).
- **Alternatives**: hardware-only tests — slow, and cannot reproduce power cuts at an arbitrary point.

## R14. The feature's boundaries regarding the IC engine

- **Decision**: this feature implements the protocol, the package format, storage, the operation dispatcher, and the **interface**
  of the IC engine (`chip_engine`), as well as an IC model for the simulator. The bit-level algorithms for
  the PADAUK families (ported from free-pdk `fpdk.c`, calibration) are a separate feature, **002-chip-engine**.
  Criterion SC-004 (programming a PFS154 in ≤ 10 s) is verified after 002.
- **Rationale**: the protocol can be fully developed and verified on the simulator; the IC algorithms
  have their own timing and hardware-validation requirements.

## R15. Setting voltages and the self-test

- **Decision**: VDD/VPP control is moved into a separate HAL `hal_power.h` (set mV, measure mV,
  enable/disable discharge), and the **settling policy** (FR-040) is implemented by the platform-independent core
  (`core/power/power.c`), used by both the IC engine and the self-test:
  1. set the voltage; remember the moment of the change;
  2. wait until the stage's minimum pause has elapsed (from the table: 100 µs / 500 µs / 1 ms / 10 ms) **and**
     `N = 3` consecutive measurements, spaced ≥ 50 µs apart, lie within ±2 % or ±30 mV (whichever is larger);
  3. if the condition is not met within 20 ms of the change → `safe_state`, `VDD_FAULT`/`VPP_FAULT`, and in `RESULT`
     — the rail, the set voltage, and the last measured voltage.
  The self-test (`SELF_TEST`) uses the same mechanism: first `PROBE` at VPP 4.5 V / VDD 2.0 V;
  an IC response → `SELFTEST_CHIP_PRESENT` without applying higher voltages; then in turn (the other rail = 0)
  the VDD and VPP levels — all distinct values from the IC table in ascending order — and 0 V after each rail;
  the result of each level (set, measured, settling time, ok) is in `RESULT.levels[]`.
  It runs automatically at start (the source is local) and on request from a PC; until a "passed" result
  exists, a local programming start is replaced by the self-test.
- **Rationale**: one implementation of the policy for all paths (programming, self-test, the simulator with a faulty-source
  model) → FR-040 and SC-011 are checked automatically; the hardware only measures.
  The minimum pause stays, because it may cover the IC's internal processes, not just the supply rail.
  The ±2 % / ±30 mV tolerance and the 20 ms timeout set the requirements for the new voltage source (settling speed
  < 20 ms, ADC accuracy better than ±1 %) — they must be taken into account in the circuit.
- **Alternatives**: checking voltages inside the `002` engine — duplication and impossible to verify without
  hardware; fixed pauses without measurement — violate principle III; a self-test only on request — the decision
  of the clarification session: automatically at start.
- **Risk**: the load of the target board during in-circuit programming (pogo) may slow VDD settling
  beyond 20 ms — then either a stronger source or a timeout parameter in the IC table (an extension
  without changing the protocol).

## R16. A local start instead of a "button"

- **Decision**: the core accepts an abstract event `local_start(request)` from the platform's controls layer
  (buttons, switches, handler signals — a separate feature). The request is turned into an operation by
  priority: (1) the self-test did not pass → `SELF_TEST`; (2) `MASTER_RESTORE_PENDING` → `RESTORE_MASTER`;
  (3) otherwise the requested operation (`PROGRAM`, `COPY` with a mode from `CONFIG`, `VERIFY`, `SELF_TEST`,
  `RESTORE_MASTER`); plus `local_cancel()`. In the protocol — `source = 1` ("local"). The full set of actions and
  indication states is fixed (FR-054, contracts/chip-engine.md §4) — the future interface only
  displays them; until then the reference board has one button → `PROGRAM`.
- **Rationale**: the protocol and the core do not depend on how many buttons and switches the programmer will have
  (a decision of the clarification session).
- **Alternatives**: single-button gestures (a long press, etc.) in the core — ties the core to a specific UI.

## R17. A failed calibration, an OTP write retry, independence from USB

- **Decision**:
  - **A failed calibration** (FR-043): after the measurement loop nothing is written — the stub
    stays a loop, the following calibrations and the fuse are skipped, and the serial number is not spent;
    `CALIB_FAILED` with the best `k` and the frequency. From a PC the `CALIB_FINALIZE_ON_FAIL` flag allows
    finishing with the best value (`warnings.CALIB_FORCED`); a local start ignores it.
  - **The retry** (FR-044) is determined only by the IC contents and the package — it may happen after
    any amount of time and on a different programmer. FLASH/MTP — a full write with an erase. OTP: the `BLANK_CHECK` stage
    accepts not only a blank IC but also one compatible with the effective image — `(content AND target) == target` for
    each word, the stub states "blank / loop / finished with an arbitrary `k`", and a serial-number stub
    blank (a new number) or written (the number from the IC).
  - **Serial numbers** (FR-046) are spent before writing (a log record before the stub words are written):
    a failed IC with a number already written keeps its number when completed by another programmer, and
    the first programmer never issues it a second time. The price is gaps in the numbering. Then the rest is appended
    (only 1→0, with a check before HV is applied — documented appending, allowed by principle III),
    unfinished stubs are calibrated, everything is verified; `warnings.RESUMED`. A fully programmed
    IC → `CHIP_ALREADY_PROGRAMMED` without changing the counters.
  - **A USB drop** (FR-023): the operation is not interrupted — the package and all parameters are already in the programmer; only
    `RUN` stops. Cancellation is only an explicit `OP_CANCEL`.
- **Rationale**: a failed OTP IC with an unfinished loop stays "rescuable": the same retry
  procedure finishes the calibration once the cause (PA3/PA4 on the board, a pogo contact) is eliminated — a separate
  "finish calibration" command is not needed. Finishing a failed stub with the "best" value
  would quietly produce a product with the wrong frequency, so it is forbidden by default. Independence from
  USB is needed for a handler and in-circuit programming: the IC must not suffer from a cable or PC
  driver glitch. The retry also covers pulling USB on a programmer powered from USB.
- **Alternatives**: a separate "finish calibration" operation — duplicates the `PROGRAM` logic; cancellation on
  a USB drop (the previous revision) — a risk of ruining an OTP through a cable glitch; a retry without a compatibility
  check — contradicts principle III.
- **Limits**: recalibrating an OTP with an already finished stub is impossible (`k` → `k'` requires 0→1;
  FR-045) — only `PATCH` to a compatible value. Compatibility is checked before HV is applied, so an incompatible
  IC is not changed.

## R18. Serial numbers: random 64 bits

- **Decision**: a serial-number stub (a free-pdk macro) always gets a **random 64-bit number**,
  separate for each IC, in all modes; there are no counters, ranges, or parameters in the package.
  Generation — the HAL `hal_entropy.h` (raw bits from hardware noise) + the core:
  1. collect ≥ 256 raw bits of entropy: on the primary MCU — a hardware RNG (a requirement for choosing the MCU); on the F072
     debug target — the low ADC bits on the temperature sensor/VREFINT and the HSI14 drift relative to HSE;
  2. mix it together with the MCU's unique ID (the 96-bit STM32 UID), the log record number, and a RAM counter
     through a cryptographic hash function (BLAKE2s or SHA-256 with small code) and take 64 bits;
  3. a source health check: raw samples are not constant and do not repeat (a repetition and proportion test
     per NIST SP 800-90B in simplified form); a source failure → the operation does not start
     (`INTERNAL`), so as not to write a predictable number.
- **Rationale**: PADAUK in its own programmer uses a rolling code with an initial value and a
  step that is reset on every file load and not synchronized between programmers —
  a source of duplicates. Random 64 bits need no coordination: the probability of at least one collision among
  10⁶ ICs is ≈ 3·10⁻⁸; mixing in the UID guarantees different sequences on different programmers
  even with a weak noise source.
- **Alternatives**: a counter with a range (the previous revisions, PADAUK rolling code) — needs
  coordination, is reset on package reload; UID + a counter alone, without entropy — predictable;
  sequential numbering for traceability — a separate feature if needed.
- **Copying** (FR-052): the reference's serial number is not recognized — firmware built by something other than free-pdk may
  have a number anywhere and of any length. Locally — a duplicate; from a PC — a search and replace on the
  user's side (read → replace with a free-pdk stub in the file → upload as an ordinary package).

## R19. Implementation: deviations from the decisions and what is verified outside the feature (T130)

A summary after implementing all the stories (as of 2026-11-26):

- **F072 firmware size** (risk R5): all features fit — `.text + .data` ≈ 28.6 KB of 44 KB, RAM
  ≈ 84.6 % of 16 KB. The options `PDK_FEATURE_MEASURE_MASTER` and `PDK_FEATURE_PATCH` remain (without them ≈ 26.7 KB);
  the build checks the limit itself (`cmake/check_size.cmake`). For `MEASURE_MASTER`, bit 9 was added to
  `INFO.capabilities` so that the client knows about a disabled feature.
- **The primary MCU** (plan.md, "Target Platform"): the candidate platform `stm32g474` is implemented (WeAct
  STM32G474CEU6 + `hardware/stm32g474-board`): 4 slots of 16 KB and the log in bank 2 of the internal flash
  (read-while-write), an 8 B write unit with ECC. The formal MCU choice, the final flash layout, and the number of
  slots are in the MCU selection specification.
- **Serial numbers** (R18): BLAKE2s-256, as planned; no deviations.
- **The `RUN` UART bridge**: receive and transmit on USART interrupts through 128 B ring buffers, not DMA
  (enough for ≤ 115200 baud, less code and RAM); the client sends `UART_TX` in 64 B chunks. On the G474 the IC's PA0 line
  goes to PB6 (FT_c, ≤ 5.0 V), so `RUN` limits VDD to 5.0 V (`hal_uart_vdd_max_mV`).
- **`MEASURE_MASTER`** (R9): the final package is built by a second streaming pass over the saved dump (RAM
  does not hold an image copy); the dump is committed as a `KEEP_VALUE` package before the first reference erase.
- **Host compiler**: gcc/clang (MinGW-w64 on Windows); MSVC is not supported (GNU extensions in the core).
- **Outside this feature**: SC-004 (programming a PFS154 ≤ 10 s) and SC-009 (copies within 1 % of the reference)
  are proven here on the `pdk-sim` model and verified on real ICs in feature `002-chip-engine`
  ([its hardware record](../002-chip-engine/hw-validation.md)): SC-004 — confirmed on a PFS154 on 2026-11-26 (4.6 s:
  erase, blank check, write, verify, one IHRC calibration; a 488-word image without a fuse word); SC-009 — not yet
  confirmed (needs several PFS154). Hardware validation of the debug target is `hw-validation.md` (T129).
- **Later change**: the F072 debug target was removed (constitution 1.5.0); the STM32G474 is the only platform. The
  F072 size figures above are historical.
