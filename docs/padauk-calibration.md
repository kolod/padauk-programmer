# Calibration of the oscillators and BandGap of PADAUK ICs

This document describes how the calibration of PADAUK ICs works in the free-pdk / Easy PDK format, and how our
programmer reproduces it. Sources: `easy-pdk-programmer-software/fpdkiccalib.c` (stub templates and
measurement loops), `easy-pdk-programmer-software/easypdkprog.c` (the calibration flow on the PC),
`easy-pdk-programmer-software/Firmware/source/Src/fpdk.c` (measurement and value search in the programmer
firmware), `easy-pdk-programmer-software/Examples/src/easypdk/pdkcommon.h` (the stub macros),
the PFS154 datasheet (comparator registers). These files are only a reference; the project code does not use them.

## 1. What is calibrated and why

| Type | Code | What is trimmed | Register | How it is checked |
|---|---|---|---|---|
| IHRC | 1 | the internal RC oscillator ~16 MHz (and the system clock divided from it) | `IHRCR` | frequency |
| ILRC | 2 | the internal low-frequency RC oscillator (~55–95 kHz) | `ILRCR` | frequency |
| BG | 3 | the BandGap reference voltage 1.20 V | `BGTR` | comparator against a VDD divider |

PADAUK's factory trimming is done by PADAUK's own programmer (the Writer) — free tools do not have
it. So free-pdk puts a **stub** into the program — a block of placeholder words that the programmer
turns into a measurement loop, calibrates, and replaces with the final code `MOV A,#k; MOV IO(reg),A`.

## 2. Stubs in the program

The program gets a stub through the macros from `pdkcommon.h`:

```c
EASY_PDK_INIT_SYSCLOCK_8MHZ();               // first choose the clock source and divider
EASY_PDK_CALIBRATE_IHRC(8000000, 4000);      // trim SYSCLK to 8 MHz at VDD = 4.0 V
EASY_PDK_CALIBRATE_ILRC(ILRC_FREQ, 4000);    // or ILRC
EASY_PDK_CALIBRATE_BG();                     // BandGap
```

Each macro is a sequence of `AND A,#k` instructions. By itself it is meaningless (which is why it is safe as a
placeholder), and the bytes `k` carry the calibration parameters:

**IHRC / ILRC** — 10 words:

| Word | `k` |
|---|---|
| 0 | `'R'` (0x52) |
| 1 | `'C'` (0x43) |
| 2 | type: 1 — IHRC, 2 — ILRC |
| 3…6 | target SYSCLK frequency, Hz, `u32` least significant byte first |
| 7…8 | calibration VDD, mV, `u16` |
| 9 | IO address of the register (`IHRCR` or `ILRCR`) |

**BandGap** — 14 words:

| Word | `k` |
|---|---|
| 0 | `'B'` (0x42) |
| 1 | `'G'` (0x47) |
| 2 | 3 |
| 3…4 | calibration VDD: always 3950 mV |
| 5 | IO address of `BGTR` |
| 6 | IO address of `GPCC` (comparator control) |
| 7 | IO address of `GPCS` (comparator divider selection) |
| 8…13 | 0 |

The `AND A,#k` opcode depends on the core width:

| Core | ICs | `AND A,#k` |
|---|---|---|
| 13 bit | PMS150C, PMS15A | `0x14kk` |
| 14 bit | PMS152, PMS154B/C, PFS154, PMS171B, PFS172 | `0x2Ckk` |
| 15 bit | PMS133/134, PFS173 | `0x54kk` |
| 16 bit | PMC251, PMS271 | `0x1Ckk` (free-pdk has no stubs for 16 bit) |

Finding a stub is a comparison with a template: the first two words are exact (`'R','C'` or `'B','G'`), the rest are
any `AND A,#*` (in BG the last six are exactly `AND A,#0`). Six or more `AND A,#*` in a row that do not
form a known stub are treated by free-pdk as an unknown stub, and it refuses to write the program;
our programmer in this case returns `PKG_CALIB_UNKNOWN` with the address (`core/chips/calib_sites.c`).

## 3. The IHRC / ILRC measurement loop

Before calibration the programmer writes a loop in place of the stub. Decoded for a 13-bit core
(for 14 and 15 bit — the same instructions with different opcodes, see the table below):

```
pos+0  SET1  PAC.3          ; PA3 — output
pos+1  SET1  PADIER.4       ; PA4 — digital input
pos+2  MOV   A, 0xFF
pos+3  ADD   A, 1           ; A = 0 — the starting value
loop:
pos+4  SET1  PA.3           ; PA3 = 1
pos+5  MOV   IO(reg), A     ; IHRCR/ILRCR = A
pos+6  T0SN  PA.4           ; PA4 = 0 → skip the next instruction
pos+7  ADD   A, 1           ; PA4 = 1 → A + 1
pos+8  SET0  PA.3           ; PA3 = 0
pos+9  GOTO  pos+4
```

- **PA3 is the frequency output.** One pass of the loop takes exactly 7 clocks regardless of PA4: the skip in `T0SN`
  costs 2 clocks, and `ADD` — 1. PA3 stays at 1 for 4 clocks and at 0 for 3 clocks, so
  `f_SYSCLK = 7 × f(PA3)` (`loopcycles = 7` in free-pdk).
- **PA4 is the "+1" input.** Once per pass the IC reads PA4; if it is 1, the value of `A` increases by 1, and from the
  next pass the oscillator runs with the new setting. The pulse must cover exactly one polling
  moment: at SYSCLK 8 MHz a pass takes 875 ns, so the pulse must be synchronous with PA3
  (see section 6).
- **The loop is endless.** PA4 does not take the IC out of the loop — it only changes the register value. The loop
  ends when the programmer removes power and rewrites the stub (section 7).

Loop opcodes (`reg` is the IO address from the stub, `p` is the stub address):

| Word | Instruction | 13 bit | 14 bit | 15 bit |
|---|---|---|---|---|
| 0 | `SET1 PAC.3` | `0x0F71` | `0x1ED1` | `0x3D91` |
| 1 | `SET1 PADIER.4` | `0x0F8D` | `0x1F0D` | `0x3E0D` |
| 2 | `MOV A,0xFF` | `0x17FF` | `0x2FFF` | `0x57FF` |
| 3 | `ADD A,1` | `0x1001` | `0x2801` | `0x5001` |
| 4 | `SET1 PA.3` | `0x0F70` | `0x1ED0` | `0x3D90` |
| 5 | `MOV IO(reg),A` | `0x0080 \| reg` | `0x0180 \| reg` | `0x0100 \| reg` |
| 6 | `T0SN PA.4` | `0x0C90` | `0x1910` | `0x3210` |
| 7 | `ADD A,1` | `0x1001` | `0x2801` | `0x5001` |
| 8 | `SET0 PA.3` | `0x0E70` | `0x1CD0` | `0x3990` |
| 9 | `GOTO p+4` | `0x1804 + p` | `0x3004 + p` | `0x6004 + p` |

## 4. The BandGap measurement loop

```
pos+0   MOV   A, 0x09
pos+1   MOV   PAC, A          ; PA0 and PA3 — outputs
pos+2   SET1  PADIER.4        ; PA4 — digital input
pos+3   MOV   A, 0x84
pos+4   MOV   IO(gpcc), A     ; comparator: on, (−) = BandGap 1.20 V, (+) = VDD divider
pos+5   MOV   A, 0xA3
pos+6   MOV   IO(gpcs), A     ; comparator output on PA0, "high" range, N = 3
pos+7   MOV   A, 0xFF
loop:
pos+8   SET1  PA.3
pos+9   MOV   IO(bgtr), A     ; BGTR = A
pos+10  T0SN  PA.4
pos+11  ADD   A, 1
pos+12  SET0  PA.3
pos+13  GOTO  pos+8
```

Decoding the comparator registers per the PFS154 datasheet:

- `GPCC = 0x84`: bit7 — enable; bits 3…1 = `010` — the negative input = BandGap 1.20 V; bit0 = 0 —
  the positive input = the internal divider `Vinternal R`.
- `GPCS = 0xA3`: bit7 — comparator output on PA0; bit5 — the "high" divider range; bits 3…0 =
  N = 3. In this range `Vinternal R = VDD · (N + 9) / 40`, that is, at VDD = 3.95 V —
  3.95 · 12 / 40 ≈ 1.185 V.

So the loop compares the BandGap with ≈1.185 V, and the result is visible on **PA0**. PA3 and PA4 work the same way
as in the IHRC loop: PA4 is "+1" to `BGTR`, PA3 is synchronization.

BG loop opcodes (14 bit; for 13 and 15 bit — similarly, see `fpdkiccalib.c`): `0x2F09, 0x0191, 0x1F0D,
0x2F84, 0x0180|gpcc, 0x2FA3, 0x0180|gpcs, 0x2FFF, 0x1ED0, 0x0180|bgtr, 0x1910, 0x2801, 0x1CD0,
0x3008 + p`.

## 5. The calibration flow

This is how Easy PDK runs it (`easypdkprog.c`, `fpdk.c`); our programmer repeats the same sequence
inside the firmware (specification `001-pc-programmer-protocol`, stage `CALIBRATE`).

1. **Write.** The program is written with all stubs replaced by measurement loops.
2. **Stub selection.** The stub with the lowest address among those not yet calibrated is calibrated.
3. **Start.** Only the calibration VDD from the stub (for example 4.0 V; 3.95 V for BG) is applied to the IC,
   without VPP — the IC starts from reset and runs the program **as usual** until it reaches the
   loop. The code before the stub runs with an uncalibrated oscillator.
4. **Value search:**
   - **IHRC**: values 0…0x9F step by step: "+1", measure the frequency, remember the one closest to the target
     (`f_SYSCLK = 7 · f(PA3)`). 0x9F is the practical maximum of `IHRCR`.
   - **ILRC**: 0…0xF0 in steps of 0x10 (only the upper 4 bits of `ILRCR` are significant): measure, then 16 "+1"
     pulses to the next step.
   - **BG**: 0…0xF0 in steps of 0x10: after each step wait 500 µs and read PA0; the first
     value at which the comparator outputs 0 (BandGap ≥ divider) is the result.
5. **Check.** For IHRC/ILRC the best frequency must be within ±10 % of the target, otherwise
   the calibration failed (in our case — `CALIB_FAILED`, policy FR-043).
6. **Finishing the stub** (section 7) and rewriting the program; VDD is removed.
7. **The next stub**: the IC starts again, passes the already finished stub, and stops in the
   loop of the next one. Repeat until no stubs remain.

If there is no signal on PA3 within 1 s (the IC did not reach the loop), the measurement failed.

## 6. Frequency measurement and synchronous pulses

### How Easy PDK does it (STM32F072)

SPI1 in **slave** mode: the IC's PA3 is the SPI clock (SCK, on PB3), the IC's PA4 is the programmer's
MISO (PB4). Mode `CPOL = 0, CPHA = 0`: the bit changes on the falling edge of PA3 and is stable when the
IC polls PA4 after the edge. SPI runs continuously through DMA (blocks of 16 bytes).

- **Frequency**: TIM2 counts 48 MHz system clocks while SPI receives N blocks; the PA3 frequency =
  `48 MHz · 8 · block_size · (N − 1) / clocks`. The window depends on the target: 24 blocks of 16 bytes (over
  4 MHz), 8 (up to 4 MHz), 4 (up to 1 MHz), 2 blocks of 4 bytes (up to 100 kHz).
- **"+1" pulses**: `(1 << n) − 1` is written to the transmit buffer — n ones in a row (n ≤ 16).
  Each SPI bit falls on exactly one loop pass, so the register grows by exactly n. After that
  one block is skipped while the oscillator settles.

### How to do it on the STM32G474 (our programmer)

| IC pin | G474 pin | Function during calibration |
|---|---|---|
| PA3 | PB3 | SPI3_SCK (AF6) — slave clock, the pulses are synchronized with the loop |
| PA3 (the same line) | PA15 | TIM2_CH1 (AF1) — edge capture for frequency measurement |
| PA4 | PC11 | SPI3_MISO (AF6) — "+1" pulses |
| PA0 | PB6 | GPIO, input — the comparator result for BG (PB6 is FT_c, up to 5.0 V only; PA9 is recommended, see the G474 README, "Permissible voltages on the pins") |

The PA3 line goes to two pins: one pin cannot be both the SPI3 clock and a TIM2 capture input at the same time,
and this way measurement and pulses run in parallel, without switching alternate functions. During
programming PB3 is an ordinary GPIO clock output.

The measurement is **reciprocal**: TIM2 (32-bit) is clocked from 170 MHz, CH1 captures the PA3 edges with an
input prescaler of ÷8 and a small digital filter; the firmware takes N = 64 captures (by polling or
DMA):

```
f(PA3) = 8 · (N − 1) · 170 MHz / (t_last − t_first)
f_SYSCLK = 7 · f(PA3)
```

At SYSCLK 8 MHz (PA3 ≈ 1.14 MHz) 504 periods take ~0.44 ms ≈ 75,000 timer clocks —
a resolution of ~0.001 %, so even 160 IHRC steps go quickly. An overcapture (the
`CC1OF` flag) means the signal is faster than the firmware can keep up with — then increase the prescaler.

The accuracy is set by the MCU's reference frequency — the HSE 8 MHz crystal (±20 ppm). If the firmware runs from the
internal HSI16 (±1 %), frequency calibration must be forbidden or an explicit warning given.

## 7. Finishing the stub

After calibration the loop is turned into the final code: all words → `NOP` (`0x0000`), except two:

| Stub | Word | Becomes |
|---|---|---|
| IHRC/ILRC | 2 (`MOV A,0xFF`) | `MOV A,#k` — the value found |
| IHRC/ILRC | 5 (`MOV IO(reg),A`) | stays |
| BG | 7 (`MOV A,0xFF`) | `MOV A,#k` |
| BG | 9 (`MOV IO(bgtr),A`) | stays |

```
; IHRC after calibration
NOP
NOP
MOV  A, #k
NOP
NOP
MOV  IHRCR, A
NOP
NOP
NOP
NOP
```

All "loop → final code" transitions only clear bits (1 → 0): `NOP` = `0x0000`, and
`MOV A,0xFF` → `MOV A,#k` changes only the low byte from `0xFF`. So finishing is possible on
**OTP** ICs too: one full program write and one rewrite of each stub. This also allows a retry
after a power loss (FR-044) — our programmer checks that in the stub's place there is either the loop or the already
final code, and writes only the needed zeros.

## 8. Which stub is active now

The programmer does not analyze the program flow and cannot see which loop the IC is in. Easy PDK simply
**assumes** that the IC executes the stubs in the order of their addresses:

1. The program is written with **all** stubs already replaced by measurement loops. Writing a loop for
   only one and leaving the rest as placeholders is not possible: on OTP the "placeholder → loop" transition needs
   0 → 1 bits.
2. The stub with the lowest address among those not yet calibrated is calibrated — the programmer assumes that the
   IC is currently in exactly that one.
3. The stub is finished, the program is rewritten, and the IC is restarted: it passes the finished
   stub and stops in the next loop.

A typical case is two stubs in a row at the start of `main`:

```c
EASY_PDK_INIT_SYSCLOCK_8MHZ();
EASY_PDK_CALIBRATE_IHRC(8000000, 4000);   // the lower address — calibrated first
EASY_PDK_CALIBRATE_ILRC(ILRC_FREQ, 4000);
```

The compiler lays out sequential code in the same order, so the execution order matches the address order.
The assumption breaks when the stubs are in different functions, in conditional branches, or the program
first calls a function with a higher address. Then the IC, say, is in the ILRC loop, the programmer
thinks it is IHRC, the "+1" pulses trim `ILRCR`, and the value found would be written to the wrong stub.

### Our programmer's checks (FR-055)

The programmer checks that the IC is executing exactly the stub it is calibrating:

1. **Frequency plausibility.** The loop is the same for IHRC and ILRC, but the frequencies differ by two orders of magnitude:
   IHRC at 8 MHz gives ≈ 1.14 MHz on PA3, ILRC ~95 kHz — ≈ 13.6 kHz. The first measurement, before any
   "+1" pulse, must lie between 1/4 and 4 of the loop's expected frequency (`target / 7`). The limits are wide,
   because the starting register value is far from the trimmed one, but they do not allow mistaking IHRC for ILRC.
2. **Response to a step.** During the search the frequency must change with each step of the value in one
   direction (allowing for measurement noise). No reaction or a chaotic change means the pulses
   are going into the wrong loop. For BandGap the programmer also checks that PA0 is driven by the IC (the comparator
   output): it switches a weak pull-up on PA0 up and down, and the level must not follow it. In the RC loops
   PA0 is an input of the IC.

A violation of any condition → code `CALIB_WRONG_SITE` (0x0415). As with `CALIB_FAILED` (FR-043), nothing more
is written, but forced finishing with the best value (`CALIB_FINALIZE_ON_FAIL`) does not
apply here: the value was found for someone else's stub, so there is no point in writing it. The client explains that the stubs
are not executed in address order.

Limitation: two stubs of the same type with close targets (for example, two IHRC) are not
distinguished by these checks. A stricter guarantee would be given by a measurement loop with a marker — two variants with different
PA3 duty cycles (4/3 and 3/4 clocks with the same 7-clock period and the same final code) that the timer
tells apart by capturing both edges. This option has not been adopted yet: it departs from the free-pdk templates.

## 9. What the program author needs to know

- **Order.** Stubs are calibrated in address order, and the IC reaches them in execution order. The program must
  execute the stubs in the same order as their addresses (usually at the start of
  `main`, in a row). Otherwise our programmer stops with `CALIB_WRONG_SITE` (section 8), and Easy PDK
  would trim the wrong register.
- **Clock before the stub.** `EASY_PDK_INIT_SYSCLOCK_*` must come **before** `EASY_PDK_CALIBRATE_IHRC`:
  the loop measures SYSCLK, not IHRC directly.
- **Code before the stub** runs with an uncalibrated oscillator and must reach the stub quickly (within
  1 s) and not wait for external events.
- **Watchdog.** The loop does not reset it. If the program enables the WDT before the stub, the IC
  will restart in the middle of the measurement.
- **Without calibration** — simply do not place the macro: then the register stays at its factory value (our
  programmer warns if the package has no stubs at all, FR-050).

## 10. Calibration traces in a dump (copying)

A program read from the IC does not contain stubs — only the final code
`NOP, NOP, MOV A,#k, NOP, NOP, MOV IO(reg),A, NOP…` (or a measurement loop, if calibration did not
finish). Our programmer recognizes such traces by the register address from the IC table and during
`COPY` replaces them with standard stubs with a determined target (specification
`001-pc-programmer-protocol`, research R9; FR-051…FR-053).

## 11. Where this is in the project code

| What | Where |
|---|---|
| Stub search, `PKG_CALIB_UNKNOWN` | `software/firmware/core/chips/calib_sites.c` |
| IO addresses of `IHRCR`/`ILRCR`/`BGTR`/`GPCC`/`GPCS` per IC | `software/firmware/core/chips/chip_table.c` |
| Measurement loop, finishing a stub | the effective image and `PROGRAM` (tasks T081, T085) |
| The engine's frequency measurement primitive | `measure_freq` (task T112) |
| Active-stub check (FR-055) | the `calibrate` primitive, tasks T131–T133 |
| G474 pin layout | `software/firmware/platform/stm32g474/README.md` |
