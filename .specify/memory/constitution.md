<!--
Sync Impact Report
==================
Version change: 1.3.0 → 1.4.0
Bump rationale: MINOR — new principle VII "English-First Engineering, Localized UI" added
(English for all code/docs/artifacts; UI localized into six languages, no hard-coded strings).
In accordance with the new principle, the whole constitution was translated from Ukrainian to
English (non-semantic; the wording of principles I–VI is unchanged in meaning).

Modified principles: I–VI — translated only, no semantic change.
Added principles:
  VII. English-First Engineering, Localized UI
Added sections: —
Removed sections: —

Templates:
  ✅ .specify/templates/plan-template.md      — Constitution Check is generated from this file
  ✅ .specify/templates/spec-template.md      — no conflicts
  ✅ .specify/templates/tasks-template.md     — added i18n note tied to principle VII
  ✅ .specify/templates/checklist-template.md — no conflicts
  ⚠ README.md / docs                         — existing Ukrainian documents are NOT yet translated;
                                                translate when next touched (see Deferred TODOs)

Deferred TODOs:
  - TODO(DOCS_TRANSLATION): existing Ukrainian-language specs, docs, and README sections predate
    principle VII and MUST be translated to English when next modified.

--- Previous revision (1.3.0) ---
Reference platform changed: primary — a higher-resource STM32 (exact model and storage layout in a
separate spec); STM32F072C8T6 (Easy PDK boards) — debug target. Principle IV wording clarified.

--- Previous revision (1.2.0) ---
Added the reference-submodule rule for free-pdk (reference only, never modified, no dependency).

--- Previous revision (1.1.0) ---
Technical Constraints extended with the licensing policy (CERN-OHL-S-2.0 hardware,
GPL-3.0-or-later software, no copying of Easy PDK hardware files, SPDX tags).

--- Previous revision (1.0.0) ---
First ratification: all principles and sections defined for the first time.
-->

# PADAUK Smart Programmer Constitution

## Core Principles

### I. Smart Programmer, Thin Client

All domain logic for programming PADAUK microcontrollers MUST run in the programmer firmware: the
IC parameter table, IC type detection, the erase → blank check → write → verify → calibrate → fuse
sequences, calibration-stub patching, serial-number insertion, and security-fuse verification.

- The PC client MUST only: parse files (Intel HEX / bin), transfer the image and parameters (IC
  type, fuse, serial number, options), start operations, and show results/progress.
- The client MUST NOT contain knowledge of timings, voltages, or algorithms of specific ICs.
- Every operation available from the client MUST also be available without it (see principle II),
  unless it needs data the programmer physically does not have.

**Rationale:** unlike Easy PDK, where the programmer is "dumb", keeping the single source of truth
about ICs in the firmware enables standalone operation and eliminates PC ⇄ firmware version drift.

### II. Standalone Operation

The programmer MUST work fully without a PC once an image has been loaded into it.

- The target firmware image together with its metadata (IC ID/type, fuse, calibration options,
  serial-number counter, CRC) MUST be stored in the programmer's internal non-volatile memory.
- The "insert IC → press button → get result" cycle MUST work without a PC; status and errors MUST
  be shown locally (LEDs and, where present, other indicators).
- Copy mode MUST be able, without a PC, to read an unprotected reference IC into memory and then
  write that image into blank ICs of the same type.
- Before every standalone write, the integrity of the stored image MUST be verified (CRC). A
  corrupted image MUST NOT be written.
- Bypassing read protection (the security fuse) is out of scope for this project.

**Rationale:** the programmer is used as a production tool at a workbench. Standalone mode is the
main differentiator of the product.

### III. Chip and Power Safety (NON-NEGOTIABLE)

- Every exit path of an operation (success, error, timeout, USB disconnect, reset) MUST remove VPP
  and VDD and put the signal lines into a high-impedance state.
- Before an operation, VDD/VPP MUST be verified to have reached the set values (ADC measurement
  with tolerance). Failure is an error; the operation MUST NOT continue.
- Voltage generation MUST have a hardware-enforced maximum that is independent of the firmware.
- A write MUST NOT start if the IC ID does not match the expected image type.
- For OTP ICs, a write MUST NOT be performed without a passed blank check (except explicitly
  documented incremental writes that only clear bits).
- Setting the security fuse MUST require explicit permission in the image metadata.

**Rationale:** OTP ICs cannot be recovered and a voltage error destroys the IC. Safety takes
priority over speed and convenience.

### IV. Hardware Abstraction and Portability

The MCU platform is replaceable: the primary one is an STM32 with sufficient resources (the model
is chosen by a separate spec); the STM32F072C8T6 on Easy PDK boards is the debug target.

- The core (IC protocol, algorithms, MCU table, storage format, command handling) MUST be
  platform-independent C with no access to registers/HAL of a specific MCU.
- Hardware access MUST go through a narrow HAL interface: IC GPIO lines, precise delays, VDD/VPP
  control and measurement, a frequency counter for calibration, non-volatile storage, the CDC
  transport, and the button/indication.
- Porting to another MCU MUST require only a new HAL implementation and a build configuration.
- The core MUST build and run on a PC (for tests and simulation) with a fake HAL.

**Rationale:** the F072 (64 KB flash / 16 KB RAM) lacks the resources for standalone storage and
new features, so a more capable STM32 will be primary; changing the MCU must not mean rewriting
the logic.

### V. Versioned PC ⇄ Programmer Protocol

- The transport is USB CDC. The protocol MUST be described in a specification under `docs/` (frame
  format, commands, error codes, timeouts) and MUST be updated together with code changes.
- Frames MUST carry an integrity check (CRC) and an explicit length. The programmer MUST return a
  response (success/error with a code) to every command.
- Both sides MUST exchange the protocol version on connect. An incompatible major version MUST be
  refused with a clear message.
- Long operations MUST report progress so that the client never has to guess timeouts.
- Protocol changes: incompatible → MAJOR, new commands/fields → MINOR.

**Rationale:** the protocol is the contract between independently updated firmware and clients.

### VI. Verifiability

- Core logic (protocol encoding/decoding, MCU table, calibration and serial-number patching, HEX
  parsing, storage format, CRC) MUST have automated unit tests that run on a PC.
- Bit-level sequences for each MCU family MUST be verified by tests on a fake HAL against
  reference traces derived from the documented timings (`docs/easy-pdk-programmer-protocol.md`).
- Support for a new IC type or family MUST be confirmed by testing on a real IC (read / write /
  verify / calibrate), with the result recorded in the feature spec.
- Documented timings are treated as minimums. Deviations MUST be justified and verified on
  hardware.

**Rationale:** protocol errors cost destroyed OTP chips; most of the logic can be verified without
hardware.

### VII. English-First Engineering, Localized UI

- All source code identifiers, code comments, commit messages, specifications, plans, task lists,
  README, and other documentation MUST be written in English, regardless of the language used in
  requests or discussions.
- The UI default language MUST be English. The UI MUST also be fully localized into Ukrainian,
  German, French, Spanish, and Japanese.
- No user-visible string MAY be hard-coded; all UI text, including error messages and confirmation
  dialogs, MUST come from resource tables. Adding a string requires adding it for all six languages
  (English text as fallback is permitted only as an explicitly tracked TODO).
- The UI language MUST follow the OS UI culture when supported, otherwise English, and MUST be
  switchable manually at runtime.
- Layouts MUST accommodate longer translations (German/French) and CJK fonts (Japanese) without
  clipping.

**Rationale:** one working language keeps the codebase maintainable; localization reaches users.

## Technical Constraints

- **Primary platform:** an STM32 with USB FS device, ≥ 128 KB flash, ≥ 32 KB RAM, VDD/VPP control
  and measurement means (DAC/ADC or equivalent), preferably hardware RNG and CRC; the exact model,
  flash layout, and number of package slots are defined by a separate spec and MUST be documented
  in its plan.
- **Debug target:** STM32F072C8T6 (Easy PDK boards, 64 KB flash, 16 KB RAM) — for verifying the
  protocol, storage, and USB before the primary hardware exists; features that do not fit MAY be
  disabled by build configuration for this target.
- **Firmware language:** C11, built with `arm-none-eabi-gcc` + Makefile/CMake, with no IDE
  lock-in. Dependencies on CubeMX-generated code MUST be confined to the HAL implementation.
- **PC client:** cross-platform (Windows, Linux, macOS), working with the OS's standard CDC driver
  and no custom drivers.
- **Image size:** up to 4096 16-bit words (the maximum among supported MCUs) plus metadata.
- **Reference knowledge:** MCU parameters, voltages, and timings come from
  `docs/easy-pdk-programmer-protocol.md`, the free-pdk sources, and the datasheets in `docs/`.
  New values MUST cite a source or a measurement protocol.
- **Licenses:** software (`software/`) — GPL-3.0-or-later (compatible with the free-pdk code);
  hardware (`hardware/`) — CERN-OHL-S-2.0. License texts are `LICENSE-SOFTWARE` and
  `LICENSE-HARDWARE` in the root (copies in subdirectories). The hardware design MUST be created
  independently: Easy PDK hardware files (CC-BY-SA-4.0) MUST NOT be copied or modified into
  `hardware/`, otherwise the design becomes a derivative and must stay under CC-BY-SA-4.0. New
  files SHOULD carry an SPDX tag (`SPDX-License-Identifier`).
- **Reference submodules:** `easy-pdk-programmer-software` and `easy-pdk-programmer-hardware` are
  included for reference only. Their contents MUST NOT be changed within this project (no edits,
  patches, or local commits inside the submodules), and the project's build and tests MUST NOT
  depend on them. Needed code is ported into `software/` preserving authorship and the GPL-3.0
  license. Moving a submodule pointer to a newer upstream commit is allowed only in a separate
  commit with an explicit justification.

## Development Process and Quality Control

- Every feature goes through the Spec Kit cycle: specify → (clarify) → plan → tasks → implement.
  The plan MUST contain a Constitution Check section covering principles I–VII.
- Every change MUST pass the firmware build, the PC core build, and all unit tests.
- Changes touching voltage generation, IC sequences, or storage MUST pass real-hardware
  verification before merging into `main`.
- Changes to the protocol or storage structure MUST be accompanied by updated documentation in
  `docs/` and a version-number bump.
- Changes to UI strings MUST update all six language resource tables in the same change (or add an
  explicitly tracked TODO for the English fallback).
- Deviations from the principles MUST be recorded in the plan's Complexity Tracking table with a
  justification.

## Governance

- The constitution takes precedence over other project practices and templates. In case of
  conflict, the template or plan is brought into line with the constitution.
- Amendments are made by changing this file with an updated Sync Impact Report and by
  synchronizing dependent templates in `.specify/templates/`.
- Constitution versioning is semantic: MAJOR — removal or incompatible redefinition of a principle;
  MINOR — a new principle/section or materially expanded guidance; PATCH — wording clarifications.
- Compliance is checked on every plan (Constitution Check) and during change review. Principle III
  cannot be waived through Complexity Tracking.

**Version**: 1.4.0 | **Ratified**: 2026-10-09 | **Last Amended**: 2026-11-26
