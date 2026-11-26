# Feature Specification: PC ⇄ programmer exchange protocol

**Feature Branch**: `001-pc-programmer-protocol`

**Created**: 2026-10-09

**Status**: Draft

**Input**: User description: "The PC ⇄ programmer exchange protocol (USB CDC). It must define: what data
is sent from the PC to the programmer (MCU type/identifier, firmware image, fuse, calibration parameters,
serial number, operation options, etc.), the exchange protocol (commands, frames), and how the programmer replies
(results, error codes, progress of long operations), given that all knowledge of how to work with a
specific chip now lives in the programmer and the PC client is thin. Data must be
transferred without errors: frame integrity checking is needed, and separately — integrity checking of the firmware
image and metadata (from the PC until it is stored in the programmer's memory and during standalone programming)."

## Clarifications

### Session 2026-10-09

- Q: How many firmware packages does the programmer store at once? → A: One active package; the second place
  is only for atomic replacement (FR-016).
- Q: Should the original Easy PDK 1.3 protocol be supported for `easypdkprog`? → A: No. A separate
  CLI client that imitates `easypdkprog` but works through the new protocol is possible (FR-030).
- Q: Can the individual calibration of the copies be restored when copying, if the reference's target is unknown?
  → A: Yes. Calibration traces in the dump are replaced by calibration stubs; the target is either measured on a
  FLASH reference (temporarily rewriting it and then restoring it), given from the PC, rounded to a
  standard frequency, or the reference's value is explicitly carried over. BandGap is always restored (FR-031…FR-036).
- Q: Should OTP programming without a blank check (`--noblankchk`) be allowed? → A: No (principle III). For
  appending individual zeros to an already programmed IC there is a separate append operation with a check
  that all changes only clear bits (FR-037, FR-038).
- Q: How do we avoid appending to the wrong IC? → A: The append mandatorily checks the checksum of all
  user memory (exact, or normalized without calibrations and serial numbers) against the expected one:
  given by the client or computed from the active package (FR-039).
- Q: Can individual calibrations be skipped, and how do we treat OTP? → A: The copy modes are separate for
  IHRC/ILRC/BG (FR-042); on a failed calibration nothing more is written, a failure signal,
  and from a PC an optional forced finish (FR-043); recalibrating a finished OTP is not
  supported (FR-045).
- Q: Are calibration-skip flags needed? → A: No. Every stub found in the firmware is calibrated; if calibration is not needed, do not place
  the macro. For OTP each calibration is one full write (with a measurement loop in place of the stub) and
  one rewrite of the stub itself (FR-006).
- Q: How do we treat drops? → A: A USB drop does not interrupt an operation (the data is uploaded before the start),
  except a run with data exchange (FR-023); after a power cut — a retry if what was written is
  compatible with the image (FR-044).
- Q: What if the reference has a measurement loop instead of a calibration trace? → A: Locally — do not
  copy, write nothing to storage, a failure signal; from a PC — ask the user: cancel or set the
  calibration target and repeat the copy with permission (FR-051).
- Q: How do we verify an IC with calibrated stubs and a serial number? → A: Verification takes stubs into account
  (FR-047); after calibration and the fuse — a final exact check of all memory (FR-048); the package CRC
  is also checked during programming and before the fuse (FR-049); a package without calibration stubs gives a warning
  (FR-050).
- Q: Is the retry in the same session? → A: No: later or with another programmer with the same package.
  FLASH/MTP — write again; OTP — a check whether zeros can be appended; an already written serial number
  is taken from the IC (FR-044).
- Q: How do we form serial numbers? → A: A random 64-bit number for all modes (FR-046). Copying does not
  process the serial number: locally — a duplicate, from a PC — search and replace on the user's side (FR-052).
- Q: Calibration in a foreign format in the reference? → A: Copy as is with a warning (FR-053).
- Q: How do we wait for VDD/VPP to settle after a change? → A: At least the documented free-pdk pause **and** until the
  measured voltage is within tolerance and stable; a timeout → a supply/programming voltage error (FR-040).
- Q: The tolerance and timeout for voltage settling? → A: ±2 % or ±30 mV (whichever is larger), a 20 ms timeout (FR-040, SC-011).
- Q: What if there is an IC in the socket during the self-test? → A: First identify the IC at
  low voltages; if it responded — a refusal with a separate code; the client additionally asks the user to confirm
  an empty socket (FR-041).
- Q: When does the self-test run? → A: Automatically at every power-up and on a command from a PC;
  on failure standalone programming is blocked (FR-041).
- Q: What if an IC is already connected at power-up? → A: Block standalone programming until the
  self-test passes; a local start in this state repeats the self-test. A typical scenario — in-circuit
  programming through pogo contacts: the programmer is powered on and checked, then the operator or a handler on the
  line presses it to the board and starts programming (FR-041).
- Q: What controls does standalone mode have? → A: Not defined yet; the number of buttons and switches
  is not limited to the original's. The protocol MUST NOT depend on specific controls: standalone actions
  are described as a "local start" of a given operation (`source` = local), and the mapping of actions to buttons,
  switches, or handler signals is defined by a separate feature.
- Q: Which local functions are implemented now, if the interface is defined later? → A: The core implements the
  full set of local actions and indication states (FR-054); any future interface only maps
  them to its controls. On the reference board, temporarily — one button → programming.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Uploading a firmware package to the programmer (Priority: P1)

A developer picks a firmware file, an IC type, a fuse, and options in the PC client and uploads all of this
to the programmer as a single "firmware package". The programmer checks that the package arrived undamaged and is
compatible with the chosen IC, stores it in non-volatile memory, and confirms the
storage. The client compares the checksum confirmed by the programmer with its own and shows the
user that the programmer holds exactly the image they chose.

**Why this priority**: without a reliably uploaded package neither programming from the PC nor standalone
operation is possible — it is the foundation of all other scenarios.

**Independent Test**: upload a package, disconnect the programmer, reconnect it, and read the
metadata and checksum of the stored package — they must match the ones the client computed.

**Acceptance Scenarios**:

1. **Given** the programmer is connected and holds no package, **When** the client uploads a valid
   package for a PFS154, **Then** the programmer stores it and returns a confirmation with a checksum
   that matches the one computed by the client.
2. **Given** part of the data is corrupted in transit, **When** the programmer receives the corrupted
   fragment, **Then** it rejects that fragment, the client resends it, and in the end the package is
   stored without errors.
3. **Given** the package is for an IC the programmer does not support, or the image is larger than the
   IC's memory, **When** the client finishes the transfer, **Then** the programmer rejects the package with a
   specific error code, and the previously stored package stays unchanged.
4. **Given** the connection dropped in the middle of transferring a new package, **When** the programmer is
   reconnected, **Then** the memory holds the previous intact package (or is empty if there was none), not a partially
   written new one.

---

### User Story 2 - Programming an IC from the PC with progress and a result (Priority: P1)

The user inserts an IC and presses "Program" in the client. The client only starts the operation;
the programmer itself determines and performs all the steps for that IC: the ID check, erase (for
FLASH), blank check, write, verify, calibration, serial number, fuse. While it works,
the client shows the current stage and the percent complete, and at the end — a summary: success or a
specific reason for failure, the address of a mismatch, the calibration results, and the serial number written.

**Why this priority**: the main usage scenario, and it verifies that all the logic of
working with the IC really lives in the programmer.

**Independent Test**: program a PFS154 from a client that has no data about ICs at all, and
make sure the IC works and the client showed all stages and the summary.

**Acceptance Scenarios**:

1. **Given** a package is stored in the programmer and a blank IC of the right type is inserted,
   **When** the client starts programming, **Then** the programmer sends progress events with the stage name and
   percent and finishes with a "success" summary with the calibration results and the serial number.
2. **Given** an IC of a different type is inserted, **When** programming starts, **Then** the operation
   stops before the programming voltages are applied with an "ID mismatch" code and the actual ID.
3. **Given** an OTP IC is already programmed, **When** programming starts, **Then** the operation
   stops at the blank-check stage with the corresponding code, and the IC is not changed.
4. **Given** programming is in progress, **When** the client sends "cancel", **Then** the programmer safely
   ends the operation (removes the voltages) and reports that the operation was cancelled and at which stage.
5. **Given** an operation is in progress, **When** the client sends another command that needs the IC,
   **Then** the programmer answers "busy" without interrupting the current operation.
6. **Given** a programmed OTP IC is inserted, **When** the client appends words that only clear
   bits, **Then** the programmer writes and verifies only those words; **When** at least one word
   needs 0→1, **Then** nothing is written, and the summary shows the address of the conflict.
7. **Given** the client passed the expected firmware checksum, and the socket holds an IC with a different
   program, **When** an append starts, **Then** the programmer writes nothing and returns the
   IC's actual checksums.
8. **Given** programming is in progress, **When** the USB connection drops (the programmer stays powered), **Then**
   programming finishes the same as without the drop, and after reconnecting the client receives the summary.
9. **Given** programming of an OTP IC was interrupted by a power cut, **When** the same IC is programmed
   again with the same package, **Then** the programmer checks that what was written is compatible with the image, appends the
   rest, and finishes the calibration; the summary marks a retry.
10. **Given** calibration failed, **When** programming ends, **Then** the programmer reports the
    failure with the best value found and writes nothing after the measurement loop; after the
    cause is eliminated, programming the same IC again finishes the calibration.

---

### User Story 3 - Standalone programming with an integrity check of the stored package (Priority: P2)

An operator without a PC connects an IC and starts programming locally (with a programmer control or a handler signal). Before every write the programmer
checks the integrity of the stored package and metadata; a corrupted package is never written to the
IC. If the firmware has a serial-number stub, each IC receives its own random
64-bit number. When the PC is connected later, the client can read the standalone operation statistics.

**Why this priority**: standalone operation is a key differentiator of the product, but it relies on
P1 (the presence of an intact package).

**Independent Test**: deliberately corrupt the stored package (with a test command or a test
firmware), do a local start — programming must not start, and the indicator must show an integrity error.

**Acceptance Scenarios**:

1. **Given** an intact package with a serial-number stub is stored, **When** the operator programs three
   ICs in a row (with a power-off between them), **Then** they receive three different random
   serial numbers.
2. **Given** the stored package is corrupted, **When** the operator does a local start, **Then** programming does not
   start, no voltages are applied, and the local indication shows an integrity error.
3. **Given** the PC is connected again after standalone work, **When** the client requests statistics,
   **Then** it receives the number of successful and failed programmings.

---

### User Story 4 - Information about the programmer and the supported ICs (Priority: P2)

On connecting, the client learns from the programmer the protocol and firmware version, the hardware
capabilities, the list of supported ICs (name, ID, memory size, type — FLASH/OTP, which
operations are available), and the state of the stored package. This way the client can show the user a list of ICs
and check the image size without a table of its own.

**Why this priority**: it is needed for the client to stay thin; otherwise it would have to duplicate the
IC table.

**Independent Test**: update the programmer firmware with a new IC in the table — the client, unchanged,
shows it in the list and can work with it.

**Acceptance Scenarios**:

1. **Given** the client and the programmer have compatible protocol versions, **When** the client connects,
   **Then** it receives the versions, capabilities, the list of ICs, and a description of the stored package.
2. **Given** the major protocol versions are incompatible, **When** the client connects, **Then** both
   sides report the incompatibility and perform no IC operations, apart from
   fetching the version.
3. **Given** the socket is empty, **When** the user starts the self-test, **Then** the client shows for
   each VDD and VPP level the set and measured voltage, the settling time, and the overall result.
4. **Given** an IC is in the socket, **When** the self-test starts, **Then** the programmer
   refuses after identifying the IC at low voltages, without applying higher ones.

---

### User Story 5 - Reading and copying an IC (Priority: P3)

The user reads an unprotected IC: either into the client (to save a file), or straight into the programmer's memory
as a new package (copying). The image obtained has a checksum that the client
verifies, and the copied package is stored with the same integrity guarantees as one uploaded
from the PC.

The reference is already calibrated: instead of calibration stubs it only has a written trim register
value that suits only this sample. So when copying, the programmer recognizes these calibration
"traces" and restores the calibration stubs in the package so that each copy is calibrated
individually. The calibration target is determined in one of these modes: measure the reference's actual frequency
(only for a rewritable reference and with explicit permission to temporarily modify it), take the target
set by the user, round to a standard frequency, or (explicitly) only carry over the reference's value.

**Why this priority**: useful for analysis and copying, but not needed for basic programming.

**Independent Test**: read a programmed IC into the client and compare with the source file;
copy an IC into the programmer and program another one with it; for a calibrated FLASH reference in the
measurement mode, the frequency of the copies must match the reference's frequency.

**Acceptance Scenarios**:

1. **Given** an unprotected IC is inserted, **When** the client requests a read, **Then** it
   receives the full image, the fuse, and a checksum that matches the one computed on the received data.
2. **Given** an IC with read protection set is inserted, **When** a read
   or a copy is requested, **Then** the programmer reports that the IC is protected and does not create a
   package.
3. **Given** a calibrated FLASH reference is inserted (for example a PFS154 with IHRC at 8 MHz) and the user
   allowed the reference to be temporarily modified, **When** copying starts in the measurement mode,
   **Then** the programmer measures the reference's actual frequency, restores the reference bit-for-bit identical to the
   original (with verification), and creates a package in which the calibration stub has the measured target.
4. **Given** the reference is an OTP IC, **When** the measurement mode is requested, **Then** the programmer
   refuses, explaining that the reference is not rewritable, and does not change the IC.
5. **Given** the user set a target frequency and voltage, **When** copying runs,
   **Then** the package has the stubs restored with that target, and the BandGap calibration is restored with its
   fixed target.
6. **Given** the power went off after the reference was erased for measurement, **When** the programmer is
   powered on again, **Then** it reports that the reference was not restored and can restore it from the
   already stored dump (from a PC or with a local start).

---

### User Story 6 - Running an IC with data exchange (Priority: P3)

A developer runs a programmed IC in the socket at a set supply voltage and sees in the client the
data the IC sends over the serial port, and can also send data to it.

**Why this priority**: convenient for debugging, but does not affect programming.

**Independent Test**: run the "hello world" example and see its output in the client.

**Acceptance Scenarios**:

1. **Given** a programmed IC is in the socket, **When** the client runs it at 5.0 V,
   **Then** the programmer reports the baud rate it detected and forwards data both ways until a
   stop command.

---

### Edge Cases

- A frame arrives corrupted, incomplete, or duplicated (after a retry) — the corrupted one
  is discarded with a request to repeat, a duplicate is recognized and not executed twice.
- The client received no response (a lost response) — a repeated request with the same number
  returns the stored response rather than executing the command again.
- The connection drops in the middle of an IC operation — the operation continues to the end (all data is already in
  the programmer); at the next connection the client receives its result. The exception is running an IC with
  data exchange, which stops.
- The programmer's power disappears in the middle of programming (including when the programmer is powered from USB and the cable
  is pulled) — after power-up a repeated programming of the same IC is possible if what was written
  does not contradict the image (FR-044).
- Calibration failed — nothing more is written to the IC (the measurement loop stays, the
  fuse is not written), a failure is signalled; the IC can be calibrated again later (FR-043).
- The reference has a measurement loop in place of calibration — copying stops before the package is stored
  (FR-051).
- A calibration stub is in the image, but the program never reaches it (no measurement signal) —
  the calibration of this stub is considered failed (FR-043); when copying in the reference measurement mode —
  the reference's value with a warning, and the other stubs are processed further.
- A power cut while storing a package — the previous package stays intact.
- A request to skip the blank check for an OTP IC — rejected; for appending to an already
  programmed OTP there is only the separate append operation with a compatibility check.
- An append needs a 0→1 bit transition in at least one word — no word is written,
  and the IC is left unchanged.
- The image goes beyond the IC's memory or overlaps reserved areas (for example, factory
  calibration values) — the package is rejected with an explanation.
- A fuse with read protection is set in the package without explicit permission — the package is rejected.
- A power or USB drop between erasing the reference and restoring it when measuring calibration —
  the reference dump is already stored, the "reference not restored" state survives power-off, and a restore
  is available as an explicit action.
- The reference dump has several calibrations (for example IHRC and ILRC) — each is measured separately, and the reference
  is restored once at the end.
- The measured reference frequency is not close to any standard one in the rounding mode — the package keeps the
  reference's value, and the summary contains a warning.
- The image contains a calibration stub of an unknown format — the package is rejected with an explanation.
- Calibration does not reach a frequency within tolerance — the operation ends with an error with the
  measured frequency; the IC is marked as failed.
- The client sends an unknown command or a command of a newer minor version — the programmer answers
  "not supported" without dropping the session.
- The self-test at power-up did not pass (a faulty supply or a connected IC) —
  standalone programming is blocked, the indication shows the reason, and a local start repeats the self-test.
- A local start is done while a PC is connected and a session is in progress — the programmer
  informs the client and does not run two operations at once.

## Requirements *(mandatory)*

### Functional Requirements

**Session and compatibility**

- **FR-001**: The programmer MUST, on request, report the protocol version (major and minor), the firmware
  version, the hardware platform identifier, and the list of capabilities.
- **FR-002**: The sides MUST refuse IC operations on an incompatible major protocol
  version; the version request MUST always work.
- **FR-003**: The programmer MUST provide a list of supported ICs with the name (and
  name variants), identifier, program memory size, word width, type
  (FLASH/OTP), available operations, and the reserved memory areas that must not be overwritten.

**Firmware package contents (data from the PC)**

- **FR-004**: The firmware package MUST contain: the target IC identifier; the program memory
  image with a marking of which words are defined and which are not; the fuse value or a "do not
  write the fuse" flag; the read-protection permission flag; operation options (skip erase,
  blank check — for FLASH only, verification; fill unused memory); the supply voltage
  for run mode;
  a **package name** (mandatory, short, for showing to the user and choosing a package), an optional description, and the
  checksum of the source file.
- **FR-005**: The package MUST NOT contain voltages, timings, or programming algorithms — the
  programmer determines them itself from the IC identifier.
- **FR-006**: Calibration parameters (the oscillator type, target frequency, voltage) MUST be determined by the
  programmer from stubs in the image itself. **Every** stub found is calibrated; there is no way to skip the
  calibration of an existing stub — to not calibrate a parameter, the developer does not place a
  macro for it (or loads the factory value with the corresponding macro).
- **FR-007**: The programmer MUST validate the package on receipt: IC support, image size,
  no overlap with reserved areas, fuse validity, and recognizability of all calibration
  stubs. A mismatch MUST
  lead to the package being rejected with a specific code and, where possible, an address.

**Transfer and integrity**

- **FR-008**: Every frame in both directions MUST have an explicit length, a sequence number, and an integrity
  check that detects any errors in a single frame up to the maximum size
  (guaranteed — all one-, two-, and three-bit errors and burst errors up to 32 bits; other
  errors — with a miss probability of at most 2⁻³²).
- **FR-009**: A corrupted or incomplete frame MUST be discarded with a message that lets the client
  retry the transfer; a frame received again with the same number MUST NOT be executed twice —
  the programmer MUST return the previous response.
- **FR-010**: The firmware package MUST have an end-to-end checksum computed by the client over the image and
  all metadata, independent of the frame checks. The programmer MUST verify it after receiving
  all the data and before storing.
- **FR-011**: Storing a new package MUST be atomic: until a successful check and write, the
  previous stored package stays valid; an unfinished store MUST NOT make the package
  corrupted or partially new.
- **FR-012**: The programmer MUST check the integrity of the stored package at power-up, before
  every IC programming (from the PC and standalone), and on the client's request; a corrupted package MUST NOT
  be used for programming.
- **FR-013**: The programmer MUST return the checksum of the stored package so that the client can
  confirm that exactly its package was stored.
- **FR-014**: An image read from an IC and passed to the client MUST be accompanied by a
  checksum.

**Storage**

- **FR-015**: The programmer MUST keep the firmware package and statistics (the number of successful and failed
  programmings) in non-volatile memory.
- **FR-016**: The programmer MUST store exactly one active firmware package. Extra space in the
  storage MAY be used only for atomic package replacement (FR-011) and MUST NOT be visible to the
  client as a second package. Uploading or copying a new package replaces the active one.
- **FR-017**: The client MUST be able to read a description of the stored package (metadata, checksum,
  integrity state), read the image itself, and delete the package.

**IC operations**

- **FR-018**: The programmer MUST support the operations: IC identification, read (into the client or
  into storage as a new package), erase (FLASH), blank check, full programming from the stored package,
  verification, running an IC with data exchange and stopping it, restoring the reference from a dump,
  appending, and a self-test of the programmer.
- **FR-019**: The "full programming" operation MUST perform all IC-specific steps in the
  programmer (ID check, erase, blank check, write, verify, calibration, serial number,
  fuse) with no client involvement between steps.
- **FR-020**: Before applying programming voltages, the programmer MUST check that the ID of the inserted
  IC matches the package.
- **FR-040**: After every change of VDD or VPP the programmer MUST continue working with the IC
  only when at least the pause documented for that stage has elapsed (as in free-pdk: 100 µs,
  500 µs, 1 ms, 10 ms) **and** the measured voltage differs from the set one by no more than ±2 % or
  ±30 mV (whichever is larger) over several consecutive measurements. If this is not reached within 20 ms of the
  change, the operation MUST end safely with a
  supply or programming voltage error, and the summary MUST contain the set and last measured
  voltage.
- **FR-041**: The programmer MUST have a **self-test** operation without an IC: sequentially set
  VDD and VPP at several levels from 0 V up to the maximums used by the IC table,
  verify by FR-040 that each level is reached and that after switch-off the voltage falls to a
  safe level; the summary MUST contain for each level the set and measured voltage, the settling
  time, and the result. Before applying any voltage above the IC identification levels, the
  programmer MUST perform IC identification; if an IC responded — the self-test MUST
  refuse with a separate code without applying higher voltages. Before starting, the client MUST ask the user to
  confirm that the socket is empty. The self-test MUST also run automatically at every
  power-up of the programmer; until the latest self-test passes, standalone programming MUST be
  blocked (with a corresponding indication), and the result MUST be visible to the client in the programmer state.
  If an IC is detected at power-up and the self-test refused, standalone programming stays
  blocked; a local programming start in this state MUST instead perform a repeated self-test,
  and only after it succeeds does the next local start perform programming.
- **FR-054**: The programmer MUST implement the following set of **local actions** (without a PC), independent of
  specific controls: programming the active package; copying (with the modes from settings);
  verifying the connected IC against the active package; the self-test; restoring the reference;
  and cancelling the current local operation. The programmer MUST report to the platform the state for indication:
  ready, an operation in progress (stage and percent), success, and a failure with a cause class — storage
  integrity, IC (ID, not blank, verification, protected), calibration, voltage/self-test,
  reference not restored, no package; and the state "local programming blocked". A future interface
  (buttons, switches, a display, handler signals) MUST expose all these actions; until it appears, one button
  for programming is enough on the reference platform.
- **FR-055**: The programmer does not analyze the program flow and calibrates stubs in address order, so it MUST
  verify that the IC is really running the measurement loop of exactly the stub it is calibrating:
  (1) **frequency plausibility** — the first measurement (before any "+1" pulse) MUST lie between
  1/4 and 4 of the loop's expected frequency for that stub (target / loop pass duration); (2) **response to a
  step** — during the search the frequency MUST change in one direction with the steps of the calibration value
  (allowing for measurement noise), and for BandGap the PA0 line MUST be driven by the IC (the comparator output), not
  floating. A failure of any condition → a separate code `CALIB_WRONG_SITE`; then FR-043 applies (nothing is
  written), and a forced finish with the best value (FR-043) MUST NOT
  be applied for this code.
- **FR-037**: For OTP ICs, full programming MUST NOT be performed without a blank check or a compatibility
  check for a retry (FR-044); a package or command that asks to skip these checks for an
  OTP IC MUST be rejected with a separate error code.
- **FR-042**: The calibration mode when copying MUST be set separately for IHRC, ILRC, and BandGap (in
  the copy command and in the local copy setting).
- **FR-043**: If calibration failed (frequency out of tolerance, no signal), the programmer MUST NOT
  write anything further — neither the finishing of the calibration stub, nor the remaining calibrations, nor the fuse — and MUST
  report the failure with the best value found and the measured frequency. A locally started programming MUST end only this way. Programming started from a PC MAY explicitly
  allow finishing the stub with the best value found (a separate flag), which is marked in the
  summary as a forced finish.
- **FR-044**: A **retry** of programming MUST be determined only by the IC contents and the package and MUST NOT
  depend on the session, time, or the programmer instance: a failed IC can be completed later by another
  programmer with the same package. For rewritable (FLASH/MTP) ICs a retry is
  an ordinary full write with an erase. For an OTP IC that is not blank, programming MUST be allowed
  only if every word already written is compatible with the package's effective image: a word needs only
  1→0 transitions to the target value; a calibration stub may be in any intermediate state —
  blank, a measurement loop, or finished; a serial-number stub — blank (then a new
  random number is generated) or already written (then **the serial number from the IC is kept**). The programmer then appends the rest, calibrates the unfinished stubs, and verifies;
  the summary marks the retry and the actual serial number. If there is nothing to write and the calibrations
  are finished — the result is "IC already programmed" without changing the counters. Incompatible contents → "IC
  not blank" with the address of the first mismatch, with nothing written.
- **FR-046**: If the firmware contains a serial-number stub (a free-pdk macro), the programmer MUST
  write into it a **random 64-bit number** generated from the programmer's hardware entropy
  source separately for each IC, in all modes (from a PC and locally). There are no serial-number
  parameters in the package, and there is no counter. On a retry, a number already written into the IC
  is kept (FR-044).
- **FR-052**: Copying MUST NOT recognize or change the reference's serial number: when copying
  locally the bytes of the number are carried over as is (the copies get the reference's number); when working with a PC, searching for and
  replacing the serial number is the user's responsibility (for example, read the reference into a file, replace the
  number with a free-pdk stub, and upload it as an ordinary package). The copy summary and the client MUST
  warn that the serial number, if there is one, will be duplicated.
- **FR-053**: If the reference dump has a write of a constant to a trim register (IHRC, ILRC, BandGap) that is
  not recognized as a free-pdk calibration trace (for example, another tool's calibration), copying
  MUST proceed as usual (the code is carried over as is), and the summary and the client MUST warn, with an address,
  that the copies will have the reference's trim value without individual calibration.
- **FR-045**: Recalibrating an OTP IC with an already finished calibration stub MUST NOT
  be supported (except the FR-044 case where the stub is not finished yet).
- **FR-047**: Comparing an IC with the package (verification) MUST take calibration stubs and
  serial-number stubs into account: in place of a calibration stub, a finished form with any
  register value is accepted, in place of a serial-number stub — any written number; the remaining words — an exact
  match. The summary MUST contain the actual calibration values and the IC's serial number, and
  a mark if some stub is not finished.
- **FR-048**: Full programming MUST end with a final check: after calibration and writing the fuse,
  the programmer MUST read all user memory and compare it exactly with the effective image, into
  which the actual calibration values and the serial number have been substituted; a mismatch → a verification error.
  The content checksums in the summary MUST be computed from this read.
- **FR-049**: The programmer MUST re-check the package checksum during programming (on the fly, from
  the data actually being written) and before writing the fuse; a mismatch → the operation stops before the fuse is written
  with a storage integrity error.
- **FR-050**: If the package has no calibration stubs at all, the programmer MUST report this in the
  storage confirmation, and the client MUST show the user a warning (the calibration macro may have been forgotten).
- **FR-038**: The programmer MUST provide a separate **append** ("patch") operation: the client specifies a list of
  words (an address and a new value, up to 64 words), the programmer checks the ID, reads the current values of those words, and
  writes only if for **every** word the new value is obtained from the current one only by
  clearing bits; otherwise nothing is written, and the summary contains the address, the current, and the desired value
  of the first conflicting word. After writing, the changed words MUST be verified. Appending to reserved
  areas is forbidden; appending a fuse that enables protection — only with explicit permission. The operation
  is available only from a PC and MUST have a "check only" mode without writing.
- **FR-039**: Before appending, the programmer MUST read all the IC's user memory
  (everything except reserved areas) and compare its checksum with the expected one passed by the client;
  on a mismatch no word MUST be written. Two kinds of checksum are supported:
  **exact** (the contents as is) and **normalized** (calibration values in calibration traces and the bytes of
  the serial number replaced with zeros), so that one expected sum fits a whole batch of ICs with the
  same firmware. The client MAY set the expected sum explicitly or ask the programmer to compute it from the
  active package ("append only to ICs programmed with this package"). The "check only" mode and
  reading the IC MUST return both sums of the current contents. After writing, the programmer MUST
  verify that all user memory equals the initial contents with the append applied.
- **FR-021**: At most one IC operation MUST run at a time; other such
  commands during an operation MUST get a "busy" response. Status, version, and cancel requests
  MUST be handled during an operation.
- **FR-022**: The client MUST be able to cancel an operation; the programmer MUST safely finish it
  (removing the voltages) and report at which stage it was stopped.
- **FR-023**: A dropped connection with the client MUST NOT interrupt an IC operation: all the data needed
  is sent before its start, and only an explicit cancel can stop the operation. The result of the last
  operation MUST be available to the client after reconnecting. The exception is running an IC with
  data exchange, which stops on a dropped connection with a safe removal of the voltages.

**Responses, progress, errors**

- **FR-024**: The programmer MUST answer every command (success or an error code) within 100 ms;
  for long operations this answer MUST mean "accepted for execution", and the summary MUST
  arrive as a separate message.
- **FR-025**: During a long operation the programmer MUST send progress messages with the
  stage name and percent complete at least once per second.
- **FR-026**: The operation summary MUST contain: the result, the stage at which it ended, the actual
  IC ID, and, depending on the operation, the address and value of the first mismatch, the calibration
  results (the register value and the frequency reached), and the serial number written.
- **FR-027**: Error codes MUST be stable and documented; each MUST unambiguously indicate the cause (corrupted frame,
  incompatible version, busy, unknown command, unsupported
  IC, invalid package, storage integrity violation, no package, ID mismatch,
  IC not blank, verification error, calibration error, IC protected, supply/programming
  voltage error, cancelled, reference not rewritable,
  reference not restored, skipping the blank check for OTP is forbidden, append conflict,
  firmware mismatch on append, an IC in the socket during the self-test, a failed
  self-test).
- **FR-028**: The programmer MUST notify the client of asynchronous events: the start and end of a
  standalone operation, and data from a running IC.
- **FR-029**: The protocol MUST be fully described in the project documentation (`docs/`) together with the
  error codes and the rules for changing versions.

**Easy PDK compatibility**

- **FR-030**: The programmer MUST NOT support the original Easy PDK 1.3 protocol; the only exchange
  protocol is the one described in this specification. Compatibility with Easy PDK tools, if needed, is
  provided on the PC side by a separate client (for example, a CLI with a command-line interface like
  `easypdkprog`) that works through this protocol. The protocol MUST provide enough operations for such a
  client to reproduce the commands `list`, `probe`, `read`, `write`, `erase`, `start` and their options.

**Calibration when copying**

- **FR-031**: When copying, the programmer MUST recognize in the reference dump the "traces" of an executed
  calibration (IHRC, ILRC, BandGap) by the placement of instructions and the address of the IC's trim register
  and, unless the "carry over the value" mode is chosen, replace them in the package with
  calibration stubs so that each copy is calibrated when programmed.
- **FR-032**: The IHRC/ILRC calibration target when copying MUST be determined (separately for IHRC and ILRC) in
  one of these modes:
  (a) **measure the reference** — the reference's actual frequency at its own register value;
  (b) **a target from the user** — a frequency and voltage set by the client; (c) **a standard frequency** —
  the copy's frequency at the reference's value, rounded to the nearest standard SYSCLK if the deviation is
  no more than 10 % (IHRC only); (d) **carry over the reference's value** without calibrating the copies. For BandGap —
  a separate choice: restore with a fixed target or carry over the reference's value. In mode (a) each stub
  is measured with a separate rewrite of the reference (the number of reference erases = the number of stubs measured + 1);
  a stub the program does not reach within 1 s gets the reference's value with a warning.
- **FR-033**: The "measure the reference" mode MUST be available only for rewritable (FLASH) references and
  only with explicit permission to temporarily modify the reference. Before the first change of the reference, its dump MUST be
  stored with the same integrity guarantees as a package; after measuring, the reference MUST be
  restored bit-for-bit identical to the dump and verified.
- **FR-034**: The state "reference modified and not restored" MUST survive power-off, be visible to the
  client and in standalone mode, and have an explicit action to restore the reference from the dump.
- **FR-035**: The calibration modes for standalone copying (without a PC) MUST be a setting
  stored in the programmer (separately IHRC, ILRC, BandGap); by default — "carry over the value" for
  IHRC/ILRC and restoring BandGap.
- **FR-036**: The copy summary MUST contain for each calibration found the type, the reference's value, the determined
  target, and the mode, as well as a warning if the target was not determined and the value was carried over.
- **FR-051**: If the reference dump has a **measurement loop** in place of a calibration (the reference is not
  calibrated — the calibration once failed or was interrupted; such an IC does not work),
  copying MUST stop **before** the package is stored: nothing is written to the programmer's storage, and
  the summary contains the address and type of each such loop. A local copy ends there with a
  failure signal. When copying from a PC, the client MUST ask the user what to do: cancel or
  set a calibration target for these stubs; in the second case the client repeats the copy with the given
  target and explicit permission, and the programmer restores a calibration stub with that target in place of the loop.

### Key Entities *(include if feature involves data)*

- **Firmware package**: everything needed to program ICs of a given type; consists of
  metadata, a program memory image, and an end-to-end checksum. It can be uploaded from the PC or
  created by copying an IC.
- **Package metadata**: the IC identifier, the fuse and protection permission, operation options, the run voltage, the name and description, the checksum of the source file, the package format version.
- **Memory image**: a sequence of program memory words with a mask of defined words.
- **IC description**: an entry in the programmer's table (name, ID, size, word width, type, reserved
  areas, available operations); passed to the client only for display and preliminary checking.
- **Frame**: the unit of exchange with a length, a sequence number, a type, data, and an integrity check.
- **Command / Response**: a client request and the programmer's response with the same sequence number.
- **Operation**: a long-running IC action with a state (running / finished / cancelled),
  stages, and a summary.
- **Event**: a message from the programmer without a request (progress, a summary, a standalone operation, data from
  the IC).
- **Statistics**: non-volatile counters of successful and failed programmings.
- **Calibration trace**: a fragment of the code of a calibrated IC (NOPs, loading a value,
  a write to a trim register at fixed positions); it gives the position, type, and value of the reference, but not the
  calibration target.
- **Reference dump**: an unmodified copy of the reference's contents, stored before any change to it; the source for
  restoring the reference.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With artificially injected errors in ≥1 % of frames during 1000 uploads of a maximum-size
  package, 100 % of uploads end with a stored image identical to the source,
  and a corrupted package is never stored.
- **SC-002**: In 100 % of cases of a corrupted stored package (a test that corrupts
  each byte of the package in turn), the programmer refuses to program an IC.
- **SC-003**: Uploading a maximum-size package that the programmer accepts for programming
  (reported to the client; on the reference platform — 8 KB), with confirmation, takes at most 3 s.
- **SC-004**: A full programming of a PFS154 with a full image (erase, check, write, verify, one
  calibration, fuse) from pressing "Program" to the summary takes at most 10 s. *In this feature — on the IC model;
  on a real PFS154 confirmed in feature `002-chip-engine` (4.6 s, 2026-11-26, see its `hw-validation.md`).*
- **SC-005**: During any operation longer than 1 s the user sees progress updates at least
  once per second.
- **SC-006**: Adding support for a new IC requires only a programmer firmware update:
  the unchanged client shows it in the list and performs all operations with it.
- **SC-007**: A power cut or USB drop at any moment of a package upload (≥100
  trials) each time leaves the programmer with either the previous or the new package, and always an intact one.
- **SC-008**: A USB connection drop (without losing the programmer's power) at any moment of programming
  (≥ 100 trials) does not change the result: the IC contents and the summary are the same as without the drop, and the
  summary is available after reconnecting.
- **SC-012**: After a power cut at any moment of programming an OTP IC (≥ 100 trials on the
  model), repeated programming of the same IC — by the same or another programmer with the same package —
  each time either succeeds with a retry mark,
  or refuses without writing anything if the contents are incompatible; a zero that is not in the effective image never appears in the IC.
- **SC-013**: On a failed calibration (a faulty oscillator model), after the operation the IC
  has no written word outside the main image and the measurement loop; repeated programming after
  a "repair" of the oscillator finishes the calibration successfully.
- **SC-009**: For a calibrated FLASH reference in the "measure the reference" mode, the IHRC frequency of the copies
  differs from the reference's frequency (measured by the same programmer) by no more than 1 %, and the
  reference after copying is bit-for-bit identical to its contents before copying. *In this feature — on the IC
  model; on real PFS154s — in feature `002-chip-engine`, not yet confirmed (see its `hw-validation.md`).*
- **SC-010**: A power cut at any moment of copying in the "measure the reference" mode (≥ 100
  trials) never leads to a loss of the reference's contents: it can always be restored from the dump.
- **SC-011**: No exchange of data with the IC begins until VDD and VPP are within
  ±2 % / ±30 mV of the set values; with an artificially faulty supply (simulation and hardware) the operation
  ends with a voltage error no later than 20 ms after the change, with nothing written to the IC.

## Assumptions

- The client parses firmware files (Intel HEX / bin) and builds the image; the programmer does not know file
  formats (constitution principle I).
- At most one client works with the programmer at a time.
- The programmer stores one active package (FR-016); the number of slots and the storage layout of the primary MCU will be
  determined after it is chosen (the next specification) — the protocol is extensible by the minor version.
- The maximum read size is 4096 16-bit words (the largest of the supported ICs); the largest
  package for writing is determined by the programmer's storage capacity (on the reference platform — 8 KB, which covers
  all ICs that support writing; the 4096-word IC is read-only).
- Calibration stubs and serial-number stubs have a format compatible with the free-pdk macros
  (`easypdk/pdkcommon.h`) so that existing programs work unchanged. The programmer finds them by searching
  for a pattern in the image, and no one passes the addresses; a stub may be anywhere, but must be **guaranteed to
  execute at program start** (free-pdk: in `_sdcc_external_startup()`) and after the clock is chosen —
  otherwise calibration will fail (FR-043). The developer places one macro for each
  parameter the program uses (IHRC, ILRC, BandGap).
- The main production use scenario is in-circuit programming (on the target board) through
  pogo contacts; the terms "IC in the socket" and "connected IC" in this specification are
  equivalent. Other board components may load VDD, which the voltage check accounts for (FR-040).
- For calibration, the IC's PA3 and PA4 lines must be free or isolated from the other circuits of the
  target board for the duration of programming; otherwise calibration may fail (FR-043).
- If the programmer is powered from USB, pulling the cable is a power cut (FR-044), not only a dropped
  connection (FR-023).
- The voltage at which the reference was calibrated is unknown from the dump; when copying, the
  voltage set by the client is used, or the typical 5.0 V.
- The erase endurance of PADAUK FLASH ICs is limited, so the "measure the reference" mode (several extra
  reference erase cycles) is never enabled without explicit permission.
- The package name is provisioned for the future: a version of the programmer with a screen and more memory will store several
  packages and show them by name; in this feature the name is only stored and returned to the client.
- The programmer has no real-time clock; timestamps, if needed, are set by the client.
- The serial number is a random 64-bit number (FR-046); sequential numbering is not supported in this feature
  (if needed — by a separate feature). The probability of a collision even for a million ICs is ~3·10⁻⁸, and no coordination
  between programmers is needed.
- The serial number of firmware not built with free-pdk may be anywhere and of any length,
  so copying does not recognize it (FR-052).
- Read protection (the security fuse) is set only with explicit permission in the package
  (constitution principle III); bypassing protection when reading is not supported.
- Since there is one package (FR-016), programming an IC from the PC always goes through uploading a package to
  storage: a CLI client in the style of `easypdkprog write` replaces the active package in the programmer.
- The concrete integrity-check algorithms, the frame format, and fragment sizes are determined at the
  planning stage; this specification sets only the requirements for error detection.
