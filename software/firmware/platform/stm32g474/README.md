# STM32G474 platform (WeAct STM32G474 core board)

Firmware for the [WeAct Studio STM32G474 core board](https://github.com/WeActStudio/WeActStudio.STM32G474CoreBoard)
with an STM32G474CEU6 (512 KB flash, 128 KB RAM) and no external flash. The board wiring is taken from the
WeAct examples. The IC engine here is a stub (`platform/common/chip_engine_stub.c`): all IC
operations answer `OP_UNSUPPORTED`.

The power driver for the `hardware/stm32g474-board` board is enabled by the build option `PDK_G474_SUPPLIES` (default
`ON`). For a bare WeAct board without the analog part it must be turned off (`-DPDK_G474_SUPPLIES=OFF`), because
otherwise the ADC inputs float and may pass the supply checks. With `OFF` the programmer reports
voltage limits of 0 V, does not set `MEASURE_POWER` in `INFO`, and any nonzero voltage ends with
`VDD_FAULT`/`VPP_FAULT`.

## Pins

| Pin | Function | Purpose | Note |
|---|---|---|---|
| PA4 | DAC1_OUT1, unbuffered | reference voltage of the VPP regulator | only with `PDK_G474_SUPPLIES`; 0…4095 → 0…13.27 V |
| PA5 | DAC1_OUT2, unbuffered | reference voltage of the VDD regulator | only with `PDK_G474_SUPPLIES`; 0…4095 → 0…6.26 V |
| PA0 | ADC1_IN1 | VDD measurement via a /6 divider | only with `PDK_G474_SUPPLIES` |
| PA1 | ADC1_IN2 | VPP measurement via a /6 divider | only with `PDK_G474_SUPPLIES` |
| PA8 | TIM1_CH1 (AF6) | gate of the +15 V boost converter switch | only with `PDK_G474_SUPPLIES`; 400 kHz, ≤ 75 % |
| PB0 | COMP4_INP0 | +15 V via a /6 divider — regulation | only with `PDK_G474_SUPPLIES` |
| PB1 | COMP1_INP1 | +15 V via a separate /14 divider — overvoltage protection | only with `PDK_G474_SUPPLIES` |
| PB4 | TIM16_CH1 (AF1) | PWM 100 kHz, 50 % → diode charge pump for −2.5 V | only with `PDK_G474_SUPPLIES`; low output speed |
| PC6 | GPIO, output | LED | active high |
| PC13 | GPIO, input with pull-down | KEY button → local programming (`PROGRAM`) | pressed = 1 |
| PA11 / PA12 | USB DM / DP | USB CDC, VID/PID `1209:0001` | |
| PA13 / PA14 | SWDIO / SWCLK | debug | |
| PF0 / PF1 | OSC_IN / OSC_OUT | 8 MHz crystal | HSI16 without a crystal |
| PB3 | GPIO / SPI3_SCK (AF6) | IC line PA3 (ICPCK): clock during programming, SPI-slave clock during calibration | FT; see "ICP connector"; the chip engine (`hal_g474_icp.c`) |
| PA15 | TIM2_CH1 (AF1), input only | the same PA3 line: edge capture for frequency measurement | FT_f; never an output; TIM2 counts the edges during calibration (the 1 MHz time base is TIM5) |
| PC11 | GPIO / SPI3_MISO (AF6) | IC line PA4: data into the OTP IC, synchronous "+1" pulses during calibration | FT_f; the chip engine |
| PB5 | GPIO | IC line PA6 (ICPDA) | FT_f; the chip engine |
| PB6 | GPIO / USART1_TX (AF7) | IC line PA0: UART in RUN mode, comparator output during BG calibration | **FT_c — up to 5.0 V only**, see "Permissible voltages"; the firmware removes the UCPD dead-battery pull-downs from PB4/PB6 |
| PB7 | GPIO / USART1_RX (AF7) | IC line PA7: UART in RUN mode | FT_f; USART1 only in `RUN` (`platform/common/usart_bridge.c`), `RUN` limits VDD to 5.0 V because of PB6 |

The DAC output buffers are off: the load is an op-amp input with a 240 kΩ pull-down, and only an
unbuffered DAC can reach 0 V (a buffered one guarantees ~0.2 V … VDDA − 0.2 V, which on VPP after amplification
would give up to ~0.85 V instead of zero). The unbuffered DAC output resistance (~10–15 kΩ) together with the 10 nF
capacitor on the pin forms an RC noise filter, and no separate resistor is needed. The 240 kΩ pull-down with this resistance gives
an error of a few percent. It, like the spread of the ADC dividers, is compensated by supply calibration
against an external voltmeter: `pdkprog calibrate` (disconnect the target board; the coefficients are stored in
the state log, see contracts/protocol.md, "Supply calibration").

The pins and scales match the schematic `hardware/stm32g474-board.dchxml`: op-amp (LM358) gain ×2 for VDD
(R3/R2 = 20k/20k) and ×4.23 for VPP (R8/R7 = 20k/6.2k), ADC dividers 100k/20k, +15 V dividers 100k/20k
(PB0) and 130k/10k (PB1). Verified on an assembled board: +15 V and −2.5 V come up, and `pdkprog calibrate`
produced VDD/VPP corrections within ±0.6 %.
Arbitrary voltages for checking with a voltmeter (after calibration, nothing connected): `pdkprog supply`
asks for VDD [VPP] interactively, and `pdkprog supply 3.3 12.5 --hold 30` holds them for 30 s and shows the
measurements every second (mean, deviation from the set value, and min..max spread). `--no-check` leaves the voltage applied even
if it does not fit the ±2 % tolerance, so that it can be compared with a voltmeter.
The lines to the PADAUK IC are described below ("ICP connector"); the chip engine of feature `002-chip-engine` drives
them (`hal_g474_icp.c`).

## ICP connector

One 8-pin connector J1 "PDK ICP" to the target board (schematic `hardware/stm32g474-board.dchxml`): the five PADAUK
in-circuit programming lines (VDD, ICVPP, ICPDA, ICPCK, GND) plus PA4, PA0, and PA7 for OTP data, calibration, BandGap,
and the RUN-mode UART. A target board with the same 8-pin "PDK ICP" header connects 1:1.

| J1 | IC pin | Net | Programmer |
|---|---|---|---|
| 1 | VDD | `Vdd` | VDD op-amp output through 20 Ω (R18) |
| 2 | PA5 / PRSTB (VPP) | `Vpp` | VPP op-amp output through 20 Ω (R17) |
| 3 | PA6 / ICPDA | `ICPDA` | PB5 |
| 4 | PA7 | `RX` | PB7 (USART1_RX) |
| 5 | PA3 / ICPCK | `ICPCK` | PB3 and PA15 |
| 6 | PA4 | `NEXT` | PC11 |
| 7 | PA0 | `TX` | PB6 (USART1_TX) |
| 8 | GND | GND | GND |

What is on each line (by family, `docs/easy-pdk-programmer-protocol.md`):

- PA3 — clock from the programmer; during calibration, the IC's frequency output.
- PA6 — data: for FLASH (PFS154, PFS172, PFS173) in both directions, for OTP — only from the IC.
- PA4 — for OTP (PMS150C, PMS152, PMS171B…) **data into the IC**; during calibration, "+1" pulses.
- PA0 — comparator output during BandGap calibration; UART programmer → IC in RUN mode;
  CLK2 for OTP3_1.
- PA7 — UART IC → programmer in RUN mode; "commit" for OTP3_1.

The PA3 line goes to two pins: PB3 is the clock during programming and the SPI3 clock during calibration, PA15 is the
TIM2 capture input. One pin cannot be both the SPI3 clock and a TIM2 input at once, and frequency measurement
and "+1" pulses run in parallel. PA15 MUST stay an input: if it drove the line together with PB3, the current
would be limited only by their two 100 Ω series resistors (≈ 16 mA).

All signal lines have a 100 Ω series resistor near the connector (as in Easy PDK): it limits the
current when the IC and the programmer drive the line at the same time. It does not protect against overvoltage — the
5 V-tolerant pins of the G474 have no diode to VDD (see below).

At VDD 5.8 V the IC has a high-level threshold of 0.7·VDD ≈ 4 V, while the programmer outputs 3.3 V. Easy PDK
works the same way (PADAUK programming mode accepts this), but it is worth verifying on hardware.

### Target board requirements

The VDD and VPP outputs are op-amp outputs with a limited current (tens of mA) behind 20 Ω, designed for ≤ 100 nF at the
connector (`hardware/README.md`). For in-circuit programming the target board MUST NOT:

- power VDD from its own source while the programmer is connected (e.g. a CR2032 in its holder): the programmer drives
  VDD to 0 V when it enters programming mode and to 5.8 V when it writes; remove the battery or put it behind a
  diode/jumper;
- connect anything that clamps PA5 (VPP) or loads it — it receives up to 11 V while VDD is still 0 V;
- load the ICP lines heavily: PADAUK recommends isolating the peripheral circuit with 10 kΩ (or 220 pF) on PA3, PA5,
  PA6 (PFS154 datasheet, "On-board Writing"). PA3 is also the frequency output during calibration, so a buzzer or LED
  on it can break calibration;
- hold a capacitor above 500 µF or a zener ≤ 5.0 V on VDD (datasheet).

A load on VPP shows as `VPP_FAULT` at the ID stage with a low measured value; `pdkprog --trace probe` shows every supply
step with its measured value. A heavy overload can trip the +15 V overvoltage protection, which stays latched until
the programmer is reset (`pdkprog info` then lists no supply capabilities).

## Permissible voltages on the pins

According to the STM32G474 datasheet (DS12288 Rev 6, table 12 "pin definition", table 14 "Voltage
characteristics", "General operating conditions"):

| Pin type | Operating VIN range | Absolute maximum |
|---|---|---|
| FT, FT_f, FT_fa (5 V-tolerant) | −0.3 … min(min(VDD, VDDA) + 3.6 V; 5.5 V) = **5.5 V** | min(VDD, VDDA) + 4.0 V ≈ 7.3 V |
| FT_c (UCPD CC lines: PB4, PB6) | −0.3 … **5.0 V** | 5.5 V |
| TT, TT_a | −0.3 … VDD + 0.3 V = 3.6 V | 4.0 V |

FT tolerance applies only in digital mode: an enabled analog function on an _a pin removes it.
FT pins have no positive injection (VIN > VDD), so a series resistor does not limit the input voltage —
the limit must be kept by the IC's supply voltage.

Pins connected to the IC:

| Pin | Type | Line | Highest voltage from the IC | Conclusion |
|---|---|---|---|---|
| PB3 | FT | PA3 | IC VDD | fine up to 5.5 V |
| PA15 | FT_f | PA3 | IC VDD | fine up to 5.5 V |
| PC11 | FT_f | PA4 | IC VDD | fine up to 5.5 V |
| PB5 | FT_f | PA6 | IC VDD | fine up to 5.5 V |
| PB7 | FT_f | PA7 | IC VDD (RUN) | fine up to 5.5 V |
| PB6 | **FT_c** | PA0 | IC VDD (RUN, BG) | fine only up to **5.0 V** |

The remaining signal pins receive only divided voltages (ADC and comparator inputs — TT_a, ≤ 2.7 V even
with 16 V on the +15 V rail) or are outputs themselves (PA4/PA5 — DAC, PA8 — gate, PB4 — charge pump).

This gives the following constraints for the firmware and the board:

- The IC sends signals to the programmer pins only at command and read voltages (2.5–3 V), during
  calibration (VDD from the stub: usually 3–5 V, 3.95 V for BG), and in RUN mode. These voltages MUST NOT
  exceed 5.5 V — the firmware must limit VDD in `RUN` and during calibration to 5.5 V (and to 5.0 V with PB6 on
  the PA0 line, until it is moved). The write voltages (up to 5.8 V) and VPP (up to 11 V, only on
  J1.2 → ADC divider) do not reach the signal pins.
- **Schematic recommendation:** move the PA0 line from PB6 (FT_c) to **PA9** (FT_fda, USART1_TX,
  AF7, free, on the "digital" side of the package). USART1 stays the same (RX is PB7), and the limit becomes 5.5 V.
  A high level on PA9 turns on the 5.1 kΩ UCPD pull-down on PB6 (dead battery), but the firmware disables it
  (`PWR_CR3.UCPD1_DBDIS`) right after start.
- After reset, PA15 has an internal pull-up (JTDI) and PB4 a pull-up (NJTRST): before initialization,
  ~3.3 V reaches the PA3 line through the PA15 series resistor via ~40 kΩ. The firmware must switch PA15 to an input
  without a pull-up as early as possible.

## Frequency measurement during calibration

The full description is in `docs/padauk-calibration.md` (sections 3–6 and 8). In short: the IC runs a
measurement loop, PA3 is a square wave of exactly 7 IC clocks (`f_SYSCLK = 7 · f(PA3)`), and PA4 is
polled once per pass — a 1 increases the calibration value by one. The "+1" pulse must cover
exactly one pass (875 ns at 8 MHz), so it is generated by hardware synchronized with PA3:

- **SPI3 in slave mode**: SCK is PB3 (clock from the IC's PA3, AF6), MISO is PC11 (to PA4, AF6),
  `CPOL = 0, CPHA = 0`. `(1 << n) − 1` is written to the transmit buffer — n ones in a row give exactly n
  steps (as in Easy PDK).
- **Frequency** by reciprocal counting on PA15: TIM2 (32-bit) from 170 MHz, CH1
  captures edges with a ÷8 prescaler; with N = 64 captures,
  `f(PA3) = 8 · (N − 1) · 170 MHz / (t_last − t_first)`. At SYSCLK 8 MHz (PA3 ≈ 1.14 MHz) this takes
  ~0.44 ms and gives a resolution of ~0.001 %. An alternative without PA15 is to count SPI3 bits and time with TIM2, like
  Easy PDK.
- No edges for 1 s — the measurement failed; the overcapture flag (`CC1OF`) means the signal is faster than the
  firmware can keep up with (increase the prescaler).
- The reference frequency is the HSE 8 MHz crystal (±20 ppm). If the firmware runs from HSI16 (±1 %), frequency
  calibration must be forbidden or an explicit warning given.
- BandGap: the IC outputs the comparator result on PA0 → PB6 as a GPIO input.
- Checks that the IC runs exactly that stub (FR-055) are in `docs/padauk-calibration.md` §8.

The chip engine implements this in feature `002-chip-engine` (user story 3: `calibrate` / `measure_freq`, the SPI3 slave
and TIM2 counter in `hal_g474_icp.c`).

## +15 V and −2.5 V rails

The rails for the output stage (the op-amp that forms VDD and VPP) are built without dedicated ICs.

**+15 V — a boost converter from USB 5 V.** A switch (a logic-level N-MOSFET,
for example AO3400 or SI2302), a 33–47 µH inductor with saturation current ≥ 0.5 A, a Schottky diode (SS14),
and a 10–22 µF / 25 V capacitor. The estimated current is up to ~25 mA (IC VDD during programming up to
20 mA, VPP, dividers, op-amp); 30–50 mA is provided for.

- Regulation is in hardware, with hysteresis: COMP4 compares the output (PB0, /6) with a threshold from the internal
  DAC3_CH2 and, while the output is higher, blanks the TIM1_CH1 pulses through OCREF_CLR. The firmware only sets the threshold
  (soft start over 20 ms) and checks that the rail came up within 50 ms.
- Overvoltage protection: COMP1 compares a separate divider (PB1, /14, for example 130 kΩ / 10 kΩ) with
  VREFINT (1.21 V) — a threshold of ≈ 17 V. When it trips, it shuts down the PWM through the TIM1 break input in hardware,
  without the program, and the PWM is not re-enabled until reset. The COMP1 configuration is locked with the LOCK bit.
- If the rail did not come up or the protection tripped, the driver keeps the VDD/VPP DAC at 0 V, and `INFO` does not
  set `MEASURE_POWER`.
- Mandatory on the board: a 10 kΩ resistor from the gate to ground (the switch is off after reset and while
  PA8 is Hi-Z) and a 16–18 V Zener diode or TVS on the +15 V rail (passive limiting if the
  whole MCU stops or the divider breaks).

**Gate circuit.** Between PA8 and the gate there is a 22–47 Ω series resistor (for example 33 Ω), placed physically next to
the switch. It damps the ringing that the gate capacitance (~0.6–1 nF for the AO3400) forms with the trace inductance,
cuts the short pin current spike when the gate is charged, and reduces noise, including on the ADC. Switching
then takes ~30–50 ns; at 400 kHz (a 2.5 µs period) the switching loss is ≈ 0.05–0.1 W at an
inductor current of ~0.3 A. The firmware sets PA8 to medium output speed, so the edges are set
by this resistor, not by the pin. If the switch gets hot, the frequency can be lowered to 200–300 kHz (`BOOST_PWM_HZ` in
`hal_g474_power.c`), but then an inductor with higher inductance is needed.

The 10 kΩ resistor to ground goes directly between the gate and the source of the switch, that is, after the
series resistor: then the switch is off even if the series resistor breaks.

```
PA8 ──[33 Ω]──┬── switch gate
              │
           [10 kΩ]
              │
GND ──────────┴── switch source
```

**−2.5 V — a diode charge pump.** PWM from PB4 through a capacitor and two Schottky diodes (BAT54S or RB521),
with 1–10 µF capacitors. The voltage is ≈ −(3.3 V − 2·Uf), the current a few milliamps — enough for the op-amp's own
consumption (~1 mA) and short output discharge pulses.

The charge pump pin is placed on the side of the package opposite the analog pins. In the UFQFPN48, PA0…PB2 (pins 8–19),
VREF+ and VDDA (20, 21) are next to each other: PWM on any of them (for example PA6 — pin 14, a neighbor of PA5 with the
VDD reference) would couple through pin and trace capacitance into the VDD/VPP references (and the output stage
amplifies them), the +15 V comparator inputs, and VDDA. PB4 is pin 42; the low output speed
further smooths the edges. On the board it is also worth adding RC filters on the DAC outputs before the op-amp,
a separate filter (ferrite or LC) on VDDA, and routing the charge pump and boost converter traces
away from the analog ones, with a separate ground return.

After reset (and in the fault handler) the firmware's first step is to drive PA8 low, stop
TIM1, and set the VDD/VPP DAC to 0 V.

Internal resources: VREFINT (ADC1_IN18, calibrated at VDDA = 3.0 V) for computing VDDA,
the hardware random number generator is the source of serial numbers, CRC is the hardware unit, TIM2 is the
microsecond timer, SysTick gives milliseconds.


## Indication

A single LED; the state is shown by the blink pattern:

| State | LED |
|---|---|
| ready (`READY`) | steady on |
| operation (`BUSY`) | 4 Hz |
| success (`SUCCESS`) | 1 Hz |
| error (`FAIL`) | 10 Hz |
| local start blocked (`BLOCKED`) | off |

Temporary display; the final indication is task T102.

## Clocking

SYSCLK 170 MHz (range 1, boost mode): HSE 8 MHz / 2 × 85 / 2 (or HSI16 / 4 × 85 / 2 if the crystal did not
start). APB1 and APB2 are 170 MHz. USB and RNG run from HSI48 with CRS trimming from SOF frames.
The ADC runs from HCLK / 4.

## Memory

| Address | Size | Contents |
|---|---|---|
| `0x08000000` | 256 KB (bank 1) | firmware |
| `0x08040000` | 4 × 16 KB | package slots 0…3 (bank 2) |
| `0x08050000` | 1 page | state log, page A |
| `0x08051000` | 1 page | state log, page B |

The firmware runs from bank 1, while the packages and the log are in bank 2. So erasing bank 2 does not
stall the CPU, and background erase is truly asynchronous. This works in dual-bank mode
(`DBANK = 1`, the factory setting) with a 2 KB page; with `DBANK = 0` the firmware works with 4 KB pages,
but erase stalls the CPU.

Writes are in 8-byte double words, each written once (ECC). A double ECC error in a word
left incomplete by a power loss causes an NMI: the handler clears the flag, and such a package is rejected by
the CRC check.

## Build

```
cmake -B build/g474 -G Ninja -DPLATFORM=stm32g474 [-DPDK_G474_SUPPLIES=OFF] \
      -DCMAKE_TOOLCHAIN_FILE=<absolute path>/software/firmware/cmake/arm-cortex-m4f.cmake software/firmware
cmake --build build/g474
```

The output is `build/g474/pdkprog-g474.elf` and `pdkprog-g474.bin`.

## Flashing via DFU

The STM32 built-in bootloader: hold BOOT0, press and release NRST, release BOOT0 —
"STM32 BOOTLOADER" appears. Then:

```
dfu-util -a 0 -s 0x08000000:leave -D pdkprog-g474.bin
```

On Windows 7 the bootloader needs the WinUSB driver (Zadig), and the programmer's COM port needs
`software/drivers/windows/padauk-programmer.inf` (it binds the built-in `usbser.sys` to `1209:0001`;
the INF is unsigned, and Windows warns once). Windows 10 and later need no drivers.
The `pdkprog` client works with Python 3.8, the last one for Windows 7.
