# Research: PADAUK chip engine for the STM32 programmers

Decisions behind [plan.md](plan.md). Sources: `docs/easy-pdk-programmer-protocol.md` (§3–§6, the free-pdk 1.3 sequences),
the free-pdk firmware in `easy-pdk-programmer-software/Firmware` (reference only), the board documentation
`software/firmware/platform/stm32g474/README.md` and `platform/stm32f072/f072.h`, and feature 001
(`specs/001-pc-programmer-protocol/contracts/chip-engine.md`).

## R1. Where the engine lives: a portable core library over a narrow HAL

- **Decision**: the bit-level algorithms are platform-independent C in `software/firmware/core/engine/`, built as a separate
  static library `pdk_engine` that implements `chip_engine.h`. It reaches the hardware only through a new HAL header
  `hal/hal_icp.h` (the IC programming lines, sub-microsecond clocking, the calibration counter) plus the existing
  `core/power` (`power_apply`) and `hal_time.h`. The STM32 firmwares link `pdk_engine` instead of
  `platform/common/chip_engine_stub.c`; the simulator `pdk-sim` keeps the IC model `platform/host/chip_model.c` as its engine.
- **Rationale**: constitution IV (porting = a new HAL), and VI (the engine is testable on a PC with a fake HAL). A separate
  library avoids a symbol clash with the model (both implement `chip_engine.h`) and lets the engine's unit tests link a fake
  HAL instead of the fake engine used by the pipeline tests.
- **Alternatives**: the engine inside each platform directory — duplicates the algorithms per MCU, violates IV; replacing the
  simulator's model with the real engine over a bit-level IC model — a large IC emulator in the simulator for little gain:
  the pipelines are already proven on the model, and the engine is proven by traces and a unit-level bit model (R8).

## R2. Bit-level primitives: in the HAL, sequences in the core

- **Decision**: `hal_icp.h` offers line-level operations that need tight timing — send `n` bits MSB-first on a chosen data line
  with a ≥ 1 µs clock-high, receive `n` bits, single clock pulses, the long OTP programming pulses on PA4 with the clock held,
  line direction / pull settings — and the core composes them into the family sequences (enter, command preamble, ACK / ID,
  read word, erase, write block, leave) exactly as documented. Every duration the core asks for is a **minimum** (FR-009).
- **Rationale**: per-bit calls through the core would cost hundreds of ns of call overhead on the 48 MHz F072 and make clock
  edges depend on the compiler; batching the shift in the HAL keeps the timing in one small, platform-tuned place while all
  protocol knowledge stays in the core (principle I/IV). The fake HAL records every call, which is what the trace tests
  compare (R7).
- **Alternatives**: the HAL exposing only "set pin / read pin" — simplest interface, but timing-fragile and slow on the F072;
  hardware SPI for the programming bit stream — the families mix data lines (PA6/PA4/PA0) and directions mid-word, which a
  peripheral handles badly.

## R3. Timing source: cycle-accurate short delays

- **Decision**: short delays (the 1 µs clock-high, 2…40 µs pulses) use a cycle counter: DWT `CYCCNT` on the Cortex-M4 G474
  (170 MHz) — the F072 option (a TIM3 at 48 MHz) went with the F072 target. Every delay waits **at least** the requested time.
  The existing `hal_delay_us()` is tightened to the same "at least" contract (today it may return up to 1 µs early because it
  counts whole ticks of a 1 MHz timer).
- **Rationale**: free-pdk's `DelayUS(1)` is ≈ 1 µs of busy loop; a 1 MHz tick cannot guarantee a 1 µs high level. Longer
  levels are harmless (documented timings are minimums), so interrupts (USB) that stretch a level are acceptable; nothing in
  the sequences has a maximum except the overall per-step budget.
- **Alternatives**: calibrated NOP loops — depend on clock and flash wait states; disabling interrupts during whole words —
  unnecessary given minimum-only timings, and risks USB timeouts.

## R4. Keeping the programming session open across primitives

- **Decision**: the engine keeps one **programming session** (IC, mode = read / write / erase, voltages applied). A primitive
  that needs the same mode on the same IC continues the open session; anything else (another mode, another primitive kind,
  `safe_state()`, an error, a cancel) leaves it. Leaving removes the voltages immediately; the mandatory pause after leaving
  (10 ms, +100 ms after erase/write) is enforced lazily — the next *enter* waits for what remains — so `safe_state()` stays
  instant. The operation manager already calls `safe_state()` at every operation end.
- **Rationale**: the `PROGRAM` pipeline writes block by block (`write_block` per 4 words): entering and leaving per block
  (≥ 110 ms each) would take ≈ 1 min for a PFS154, far beyond SC-001's 10 s. The contract (guarantee 3) already allows "the
  next step of the operation explicitly continues the session". Between blocks the pipeline calls `poll()`, so the session
  stays responsive (FR-007).
- **Alternatives**: a bulk "write image" primitive — would change the core ⇄ engine interface of feature 001 and the
  pipelines; entering per block — too slow.

## R5. ID before high voltage

- **Decision**: two layers. (1) The pipelines already run `probe` (VPP 4.5 V / VDD 2.0 V, free-pdk §4.6) at the `ID` stage
  before any erase or write and compare the ID with the package. (2) Every engine session additionally checks the ID at the
  family's *command* voltages before raising to the operation voltages: FLASH — the 12-bit ACK of the command; OTP — the
  `GetID` pass (the write command at command voltages, N bits received, leave before any programming voltage, §4.5).
  A mismatch ends the primitive with `CHIP_ID_MISMATCH`, no answer with `CHIP_NOT_FOUND`.
- **Rationale**: FR-004 and principle III; layer 2 also catches an IC swapped between stages.
- **Alternatives**: trusting the pipeline's probe only — misses a swap between stages.

## R6. Voltages: through `power_apply`, never faster than documented

- **Decision**: every voltage change goes through `power_apply()` (feature 001 FR-040: settle within ±2 % / ±30 mV, 20 ms
  timeout), with the free-pdk pause as the minimum: enter — VPP first, ≥ 100 µs, then VDD, ≥ 500 µs; operation voltages —
  ≥ 1 ms each for reading, ≥ 10 ms each for write/erase. PFS172 gets VPP = 0 during operations (chip table). A power fault
  ends the primitive through the safe state with `VDD_FAULT` / `VPP_FAULT`.
- **Rationale**: FR-005, principle III; the pause and the settle check combine "documented minimum" with "measured".

## R7. Reference traces: an independent generator, compared by the unit tests

- **Decision**: `software/tests/vectors/gen_traces.py` (stdlib Python, written from `docs/easy-pdk-programmer-protocol.md`
  independently of the C code) emits, per family and operation, the expected sequence of HAL events (voltage set points,
  line direction changes, sent / received bit groups with their line, clock pulses and minimum durations) as JSON. The fake
  HAL in the C tests records the same events; `tests/test_engine_trace.c` compares them event by event, durations as
  "at least". Covered: probe; read, erase, write block, fuse write for FLASH_1, FLASH_2, OTP1_2, OTP2_1, OTP2_2; read and
  GetID for OTP3_1 (read-only).
- **Rationale**: constitution VI ("verified on a fake HAL against reference traces derived from the documented timings");
  the generator is the second, independent reading of the document — the same approach as the footprint vectors of
  feature 001 (`gen_footprints.py`).
- **Alternatives**: hand-written expected arrays in C — not independent of the implementer's reading.

## R8. Functional tests: a unit-level bit model of the IC

- **Decision**: `tests/fakes/fake_icp_ic.c` decodes the bit stream on the fake lines for FLASH_1/FLASH_2/OTP families
  (command preamble, ACK / ID, address, data, programming pulses, erase clocks) and keeps a memory with OTP/FLASH semantics,
  so the engine's primitives can be checked end to end without hardware: write then read back, erase, ID mismatch, a missing
  IC (no ACK), a weak cell, cancel mid-write. Calibration is tested with a fake counter that reports a frequency curve
  `f(k)`.
- **Rationale**: traces prove the order of events, the bit model proves that what is written is what is read; together they
  cover FR-009/FR-010 and catch off-by-one bit counts that a trace written by the same hand could share.

## R9. Calibration hardware: counting loop turns and shifting "+1" pulses

- **Decision**: the HAL provides `calibration` operations: start (VDD only, PA3 as the IC's output into the programmer, PA4
  driven by the programmer), shift exactly `n` one-bits to PA4 synchronously with the IC's PA3 edges, and count PA3 edges
  over a measured window. The core computes `SYSCLK = edges / window × 7` (the loop is 7 clocks per turn) and runs the search
  and the FR-055 checks.
  - **G474**: SPI3 in slave mode — SCK = PB3 ← IC PA3, MISO = PC11 → IC PA4 — with a DMA transmit buffer of one-bits for the
    "+1" pulses (one SPI bit = one loop turn), and TIM2 in external-clock mode on TI1 = PA15 (the same PA3 line, input only)
    counting edges; the window is timed by DWT. The 1 MHz system time base moves from TIM2 to TIM5 (32-bit) to free TIM2.
    This is exactly the routing the board was designed for (stm32g474 README, "ICP connector").
  - **Windows**: chosen by the expected frequency so that one count is ≤ 0.05 % (IHRC 8 MHz: PA3 ≈ 1.14 MHz → 10 ms ≈
    11 400 counts; ILRC ≈ 62.5 kHz SYSCLK → PA3 ≈ 9 kHz → ≥ 200 ms per point for 0.05 %). A window never exceeds the 100 ms
    step budget: longer windows are accumulated over several polled steps.
  - **Search**: IHRC — trim 1…0x9F in steps of 1, closest to the target; ILRC — 0x00…0xF0 in steps of 0x10; BandGap — 0x00…0xF0
    in steps of 0x10, 500 µs settle, the first value where the IC's PA0 (comparator) reads 0 (free-pdk §6.3), plus the FR-055
    plausibility / step-response / "PA0 driven" checks of feature 001. MEASURE_MASTER — no pulses, one window, "no signal" after
    1 s; SNAP_STANDARD — measure at the master's value, then search as in feature 001.
- **Rationale**: free-pdk's method is proven; a hardware counter is required (≈ 2.3 MHz edges at 16 MHz SYSCLK is too fast
  for interrupts). The G474 board has a dedicated capture pin, so counting and pulsing run on separate peripherals there.
- **Alternatives**: counting with interrupts — too fast; TIM input capture on PB3 on the G474 — PB3 cannot be the SPI3 clock
  and a TIM input at once (the reason PA15 exists).

## R10. Pin voltage limits during calibration and run

- **Decision**: the engine refuses a calibration or run whose VDD exceeds the HAL's limit for the IC-driven lines
  (`hal_icp` reports it; G474 = 5.0 V because PA0 goes to the FT_c pin PB6; F072 = no extra limit). The user program runs
  before the loop is reached and may drive PA0 at VDD.
- **Rationale**: FR-008; generalises the 5.0 V run limit added in feature 001 (`hal_uart_vdd_max_mV`) to calibration.

## R11. Performance budget (SC-001, PFS154)

| Step | Estimate |
|---|---|
| probe + session entries (incl. post-leave pauses) | ≈ 0.4 s |
| erase (2 clocks × 5 ms + 2 × 10 ms ramps + 100 ms) | ≈ 0.15 s |
| blank check / verify / final verify: 3 × 2048 words × ≈ 45 µs | ≈ 0.3 s |
| write: 512 blocks × ≈ (≈ 70 bits + 8 × 30 µs + 25 µs + 100 µs) | ≈ 0.3 s |
| IHRC calibration: up to 159 steps × ≈ 10 ms window | ≈ 1.6–2 s |
| fuse write + finished stub write | ≈ 0.3 s |
| **total** | **≈ 3–4 s** (budget 10 s) |

## R12. Debug-target size (superseded: the engine targets the G474 only)

- **Decision**: the engine is expected to add ≈ 6–8 KB of flash and < 1 KB of RAM (DMA buffers 2 × 16 B, session state);
  the size check of feature 001 guards the F072.
- **Correction (implementation, 2026-11-26)**: the 28.6 KB measured in feature 001 was too low. With the stub engine every
  read "failed", so LTO removed everything behind it (`COPY`'s package building, `snprintf` with newlib's malloc). With the
  real engine's read and probe the F072 firmware is ≈ 39.6 KB (the `snprintf` calls were replaced by a tiny formatter,
  −2 KB). The remaining ≈ 4.4 KB will not hold write, erase, and calibration with all features, so the F072 build is
  expected to turn `PDK_FEATURE_PATCH` and/or `PDK_FEATURE_MEASURE_MASTER` off (constitution: features MAY be disabled on
  the debug target; `INFO.capabilities` reports it).
- **Superseded (2026-11-26)**: the STM32F072 target was removed from the project (constitution 1.5.0); the engine targets
  only the WeAct STM32G474 board, so no debug-target budget constrains it.

## R13. What only hardware can settle (recorded in hw-validation)

- writing at VDD 5.8 V with 3.3 V logic (the IC's input threshold ≈ 0.7·VDD, as on Easy PDK);
- the real clock-high/low widths and edge quality through the 100 Ω series resistors (logic analyzer);
- calibration accuracy against an external frequency counter (SC-003) and MEASURE_MASTER copies (SC-002);
- voltage behaviour with no IC / a reversed IC and the switch-off time (SC-005, SC-006);
- interoperability with an Easy PDK programmer (SC-008).
