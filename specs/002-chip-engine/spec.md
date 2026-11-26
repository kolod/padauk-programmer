# Feature Specification: PADAUK chip engine for the STM32 programmers

**Feature Branch**: `002-chip-engine`

**Created**: 2026-11-26

**Status**: Draft

## Clarifications

### Session 2026-11-26

- Q: Which PADAUK ICs are available for hardware verification? → A: Only the PFS154. The other families are implemented
  and trace-tested but marked "not yet confirmed on hardware" (FR-017); OTP stories (US4) are verified on the IC model and
  traces only.
- Q: Which programmer boards does the engine target? → A: Only the WeAct STM32G474 board. The STM32F072 (Easy PDK) target
  is retired and removed from the project (constitution 1.5.0); the former user story 6 (an Easy PDK port) is dropped.

**Input**: User description: "002-chip-engine: the PADAUK chip engine for STM32 (main board — WeAct STM32G474 +
hardware/stm32g474-board, debug target — STM32F072 Easy PDK). Implement the bit-level algorithms of the chip_engine.h
interface from feature 001 instead of the stub: probe (ID detection at low voltages), read, erase (FLASH), write_block,
calibrate (IHRC/ILRC/BandGap through the measurement loop and pulses on PA4, the FR-055 check), measure_freq
(MEASURE_MASTER), run_start/run_stop, safe_state — for the families in the chip table (PFS154/PFS172/PFS173,
PMS150C/PMS152/PMS171B, etc.), based on the free-pdk fpdk.c algorithms (easy-pdk-programmer-software in the
repository, reference only). The guarantees of the chip-engine.md contract: ID check before high voltage, voltages
confirmed before every bit, safe exit, atomic steps < 100 ms, cancellation. Criteria: SC-004 (programming a PFS154
≤ 10 s) and SC-009 (copies within 1 % of the reference) on real ICs; hardware verification with a logic analyzer /
oscilloscope."

## Context

Feature `001-pc-programmer-protocol` delivered the protocol, the package storage, the operation pipelines (`PROGRAM`,
`VERIFY`, `READ`, `COPY`, `PATCH`, `RUN`, `SELF_TEST`, …), local operation, and the interface between the core and the
**chip engine** — the part that actually talks to a PADAUK IC over its programming lines. On the STM32 boards the engine
is still a stub that answers "not supported", so the programmer can store packages but cannot touch a real IC. All
pipelines were proven only against the IC model of the simulator.

This feature replaces the stub with a real engine on the programmer boards, so that every operation of feature 001 works
on real ICs without changing the protocol, the client, or the pipelines.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Identify and read a real IC (Priority: P1)

A user inserts a PADAUK IC into the target board connected to the programmer and asks the programmer what is there
(`probe`) and to read it into a file (`read`). The programmer applies only the low identification voltages, recognises
the IC type from its ID, reads all of its memory, and returns it together with the checksums.

**Why this priority**: identification and reading use the lowest voltages, never change the IC, and exercise every
signal line, timing, and voltage step the other operations need. They are the safe first contact with real silicon and
the base of `VERIFY`, `COPY`, `PATCH`, and of the ID check before every write.

**Independent Test**: insert a known IC (a PFS154 programmed by another programmer, and a blank PMS150C), run `probe`
and `read`; the reported type matches the marking, and the read image matches the original firmware (or is all blank).

**Acceptance Scenarios**:

1. **Given** a PFS154 in the socket, **When** the user runs `probe`, **Then** the programmer reports PFS154 (ID 0xAA1) and
   the FLASH family, and the voltages on the IC never exceed the identification levels of the chip table.
2. **Given** an IC programmed with a known image, **When** the user runs `read`, **Then** the file equals the image word for
   word, and repeated reads give identical data and checksums.
3. **Given** an empty socket, a reversed IC, or an IC type that is not in the chip table, **When** the user runs `probe` or
   any operation, **Then** the programmer reports "no IC" / "ID mismatch" without ever applying a write or erase voltage.
4. **Given** a read-protected IC, **When** the user reads it, **Then** the programmer reports that the IC is protected
   (as defined in feature 001), and nothing is changed.

---

### User Story 2 - Program a FLASH IC (Priority: P1)

A user writes a firmware into a FLASH IC (PFS154, PFS173, PFS172) from the PC or with the programmer's button: erase,
blank check, write, verify, fuse, final verify — the `PROGRAM` pipeline of feature 001, now on real silicon.

**Why this priority**: FLASH ICs are reusable, so all write timings can be tuned on them without destroying parts; this is
the main daily use of the programmer.

**Independent Test**: program a PFS154 with a test image (without calibration), read it back with this programmer and
with a reference programmer (Easy PDK), and run the program on the target board.

**Acceptance Scenarios**:

1. **Given** a stored package for PFS154 and a PFS154 in the socket, **When** the user starts `PROGRAM`, **Then** the IC is
   erased, written, verified, gets its fuse, passes the final verify, and runs the firmware.
2. **Given** a PFS154 that already holds another firmware, **When** the user programs it, **Then** the old contents are gone
   and the new image is written exactly.
3. **Given** a FLASH IC whose write does not hold (a weak cell or wrong timing), **When** the user programs it, **Then** the
   programmer reports a verify failure with the address and the expected / actual word.
4. **Given** the user cancels or the IC is pulled out during an operation, **When** the operation stops, **Then** all
   voltages are removed and the signal lines are released at once.

---

### User Story 3 - Calibrate oscillators on a real IC (Priority: P2)

A firmware with free-pdk calibration stubs (IHRC, ILRC, BandGap) is programmed; the programmer runs each measurement
loop, finds the trim value that gives the target frequency (or the BandGap reference voltage), and finishes the stub —
on real silicon. The same measurement serves `COPY` with "measure the reference" (MEASURE_MASTER) and SNAP_STANDARD.

**Why this priority**: calibration is what makes PADAUK parts usable (the uncalibrated oscillator may be off by tens of
percent), and it is the key difference from a plain programmer; it builds on Story 2.

**Independent Test**: program `helloworld` with an IHRC 8 MHz stub into a PFS154, then measure the IC's clock with a
frequency counter or oscilloscope at the calibration voltage.

**Acceptance Scenarios**:

1. **Given** a package with an IHRC 8 MHz stub, **When** the user programs a PFS154, **Then** the result shows the found trim
   value and measured frequency, and an external instrument confirms the IC runs at 8 MHz within the stated accuracy.
2. **Given** a package with ILRC and BandGap stubs, **When** the user programs the IC, **Then** every stub is calibrated in
   address order and finished.
3. **Given** a measurement loop that is not the one being calibrated, a broken oscillator, or a line not driven by the IC,
   **When** calibration runs, **Then** the programmer refuses with "wrong site" or "calibration failed" (feature 001,
   FR-043, FR-055) and leaves the loop unfinished for a later retry.
4. **Given** a calibrated FLASH reference, **When** the user copies it with "measure the reference" and programs copies,
   **Then** each copy runs within 1 % of the reference's frequency, and the reference is bit-for-bit unchanged.

---

### User Story 4 - Program an OTP IC (Priority: P2)

A user programs one-time-programmable ICs (PMS150C, PMS152, PMS171B, PMS154B/C): blank check, write, calibration, fuse,
including a retry after an interrupted write (feature 001, FR-044).

**Why this priority**: OTP parts are cheap and common in production, but every mistake destroys a part, so they come
after the FLASH timings are proven.

**Independent Test**: program a PMS150C with a calibrated test image; read it back; interrupt a second part mid-write
(power off) and finish it with a retry. *No OTP IC is on hand for this feature (FR-017): until one is, this story is verified
by the reference traces and the IC model, and OTP writing is documented as not yet confirmed on hardware.*

**Acceptance Scenarios**:

1. **Given** a blank PMS150C and a stored package, **When** the user programs it, **Then** it is written, calibrated,
   fused, verified, and runs.
2. **Given** a PMS150C whose write was interrupted, **When** the user programs it again with the same package, **Then** the
   programmer finishes it (`RESUMED`) or refuses it as incompatible without writing.
3. **Given** a non-blank OTP IC with incompatible contents, **When** the user tries to program it, **Then** no write voltage
   is applied.

---

### User Story 5 - Run the IC with the UART bridge (Priority: P3)

A user runs a programmed IC on the target board (`pdkprog start`) and talks to it through the bridge of feature 001.

**Why this priority**: the bridge itself exists; this story only makes the engine hand the IC lines over correctly (power on
VDD only, programming lines released) on real hardware.

**Independent Test**: program a "hello" firmware that sends `U` + text on PA7, run `start`, see the text, type characters
and see them echoed.

**Acceptance Scenarios**:

1. **Given** a programmed IC, **When** the user runs `start -r 3.3`, **Then** the IC runs at 3.3 V with VPP off, the bridge
   reports the speed and the text.
2. **Given** a running IC, **When** the user presses Esc or unplugs USB, **Then** VDD drops at once.

---

### Edge Cases

- No IC, a reversed IC, a short between lines, or a damaged IC: identification fails at low voltage; no write or erase
  voltage is ever applied; the supply check of feature 001 catches an overload.
- The IC is pulled out or the target board is disconnected in the middle of a write: the next step fails (verify, ACK, or
  the supply check), voltages go off, the operation ends with an error; a FLASH IC can be reprogrammed, an OTP IC goes
  through the retry rules of feature 001.
- PFS172 needs no VPP during its operations (the chip table gives 0 V): the engine MUST NOT apply VPP to it.
- At VDD 5.8 V the IC's high input threshold (~0.7·VDD) is above the programmer's 3.3 V logic level, as on Easy PDK;
  writing at this voltage MUST be confirmed on hardware.
- The IC output line seen by a pin that tolerates only 5.0 V (PA0 on the main board): the engine MUST NOT drive that IC
  above 5.0 V while the pin is connected to it (BandGap calibration at 3.95 V is fine).
- A measurement loop that never starts (the program does not reach it) or a frequency outside the measurable range:
  calibration ends with "calibration failed" / MEASURE_MASTER treats it as "no signal" (feature 001).
- A cancel request arrives during a long step (erase, a write block, a frequency measurement): the step finishes within
  100 ms and the operation ends safely.
- Families that the chip table marks read-only (PMS131, PMS132, PMS133, PMC251, PMS271, MCU390): identification and
  reading work, writing is refused as in feature 001.

## Requirements *(mandatory)*

### Functional Requirements

**Coverage**

- **FR-001**: The engine MUST implement, on the main programmer board, every primitive of the core ⇄ engine interface defined
  by feature 001 (identify, read, erase, write a block, calibrate, measure a frequency, run, stop, safe state), so that
  every operation of feature 001 works on real ICs without changes to the protocol, the client, or the operation pipelines.
- **FR-002**: Identification and reading MUST work for every IC family in the chip table, including the read-only ones;
  erase applies to FLASH families; writing and calibration MUST work for every IC the chip table marks writable.
- **FR-003**: The per-IC parameters (voltages, address/data widths, block sizes, erase clocks, reserved areas, family) MUST
  come from the chip table of feature 001; the engine MUST NOT keep a second copy of them.

**Safety (constitution principle III, contract guarantees)**

- **FR-004**: No write or erase voltage MAY be applied before the IC's ID, read at identification voltages, matches the
  expected type; a mismatch or no answer MUST end the primitive with "ID mismatch" / "no IC".
- **FR-005**: Every voltage change MUST be confirmed by the supply check of feature 001 (FR-040) before the next bit is sent
  to the IC.
- **FR-006**: Every primitive MUST leave the IC in the safe state on every exit path — VDD and VPP removed and all programming
  lines high-impedance — unless the next step of the same operation explicitly continues the programming session.
- **FR-007**: No atomic step MAY keep the programmer from serving the PC session and the cancel request for longer than
  100 ms; a cancel MUST end the primitive within that time through the safe state.
- **FR-008**: The engine MUST NOT drive a programmer pin above its rated voltage, and MUST NOT drive a line that the IC is
  driving at the same time except through the series resistors foreseen by the hardware.

**Timing and signals**

- **FR-009**: The bit-level sequences (entering and leaving programming mode, the command preamble, identification, reading,
  erasing, writing, the fuse) MUST follow the documented free-pdk sequences for each family
  (`docs/easy-pdk-programmer-protocol.md`), treating documented timings as minimums (constitution VI); any deviation MUST
  be justified and confirmed on hardware.
- **FR-010**: For each family, the generated bit sequence MUST be checkable without hardware against a reference trace derived
  from the documented sequence (constitution VI).
- **FR-011**: Reading MUST deliver data at a rate that keeps a full read of the largest supported IC within the timing budget
  of SC-001.

**Calibration**

- **FR-012**: Calibration MUST measure the IC's output frequency on the clock line while stepping the trim value with "+1"
  pulses, as the free-pdk measurement loop expects, and MUST perform the plausibility and step-response checks of feature 001
  (FR-055) on real measurements.
- **FR-013**: BandGap calibration MUST read the IC's comparator output and MUST confirm that the IC actually drives that line.
- **FR-014**: The frequency measurement MUST serve MEASURE_MASTER (a measurement without "+1" pulses, ending with "no signal"
  after 1 s) and SNAP_STANDARD as defined by feature 001.

**Run and portability**

- **FR-015**: Starting a run MUST apply VDD only (VPP removed) and release the programming lines for the UART bridge of
  feature 001; stopping MUST return to the safe state.
- **FR-016**: The engine's algorithms MUST live in the platform-independent core and reach the hardware only through the HAL
  (constitution IV), so that another programmer board would need only its own HAL. The engine targets the WeAct STM32G474
  board, the only firmware hardware target.
- **FR-017**: Each family's support MUST be confirmed on a real IC (identify, read, write / verify where writable, calibrate)
  with the result recorded in this feature (constitution VI); a family not yet confirmed MUST be marked as such in the
  documentation. The only IC on hand for this feature is the **PFS154** (FLASH_1): it is confirmed on hardware; the other
  families are implemented and checked against their reference traces (FR-010) but documented as "not yet confirmed on
  hardware" until a real IC is tested.

### Key Entities

- **IC programming session**: the time between entering and leaving programming mode of one IC: the applied voltages,
  the family-specific mode, and the guarantee that every exit removes them.
- **Family sequence**: the bit-level recipe of one IC family (how to enter the mode, send a command, read and write words,
  erase, set the fuse), parametrised by the chip table.
- **Reference trace**: the expected sequence of line levels and voltage changes for an operation of a family, derived from the
  documentation, used to check the engine without hardware.
- **Frequency measurement**: the counted edges of the IC's clock output over a gate time, with the "+1" pulse train that
  steps the trim value, giving the frequency for a trim value.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A full programming of a PFS154 with a full image (erase, blank check, write, verify, one IHRC calibration, fuse,
  final verify) takes at most 10 s from the start to the summary (feature 001, SC-004).
- **SC-002**: For a calibrated FLASH reference copied with "measure the reference", the IHRC frequency of three copies with
  different oscillator spreads differs from the reference's frequency (measured by the same programmer) by no more than 1 %,
  and the reference stays bit-for-bit identical (feature 001, SC-009).
- **SC-003**: After IHRC calibration to 8 MHz, the IC's frequency measured with an external instrument at the calibration
  voltage is within ±1 % of the target for every calibrated IC.
- **SC-004**: 100 consecutive reads of the same IC give identical data and checksums (no bit errors).
- **SC-005**: In 100 % of observed cases (oscilloscope, at least 20 attempts per case) with no IC, a reversed IC, or an ID that
  does not match, no voltage above the identification levels reaches the IC.
- **SC-006**: After any operation end — success, error, cancel, USB disconnect — VDD and VPP at the IC fall below 0.3 V
  within 10 ms and the programming lines are released (oscilloscope).
- **SC-007**: The bit sequence of every implemented family matches its reference trace in automated tests without hardware.
- **SC-008**: An IC programmed by this programmer reads back identically with the reference Easy PDK programmer, and an IC
  programmed by Easy PDK reads back identically with this programmer.

## Assumptions

- The main board is the WeAct STM32G474 module with `hardware/stm32g474-board` (supplies, ICP connector, the line mapping in
  `software/firmware/platform/stm32g474/README.md`); it is assembled and its supplies are verified. It is the only firmware
  hardware target; the former STM32F072 (Easy PDK) target was removed.
- The free-pdk sources in `easy-pdk-programmer-software` are a reference only (constitution): needed algorithms are ported
  into `software/` keeping authorship and GPL-3.0; the build does not depend on the submodule.
- Feature 001's protocol, chip table, pipelines, and the IC model of the simulator stay as they are; the model remains the
  test oracle for the pipelines, while this feature adds trace-level tests of the real engine.
- A logic analyzer and an oscilloscope are available for hardware verification; a reference Easy PDK programmer is available
  for SC-008 (if not, SC-008 compares against images built by the free-pdk toolchain instead).
- PFC154 (absent from the chip table) and new IC types are out of scope; adding them is a separate feature.
- The VDD ADC wander observed on the first board (supply reading, not the rail) does not affect IC operations, since the
  supply check uses a tolerance; it is re-checked on the second board.
