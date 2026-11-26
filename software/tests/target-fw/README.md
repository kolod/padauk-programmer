# Test programs for the PADAUK ICs

Small programs that run **on the target IC** to check the programmer on hardware (feature `002-chip-engine`,
[quickstart](../../../specs/002-chip-engine/quickstart.md)).

## Build

Requires SDCC ≥ 4.1 (the `pdk14` port) and the free-pdk headers (`pdk/device.h`, `easy-pdk/calibrate.h`). The headers
are not in this repository: the script takes them from `$PDK_INCLUDES`, or from the PlatformIO easypdk framework
(`~/.platformio/packages/framework-easypdk/includes`).

```sh
python software/tests/target-fw/build.py            # → build/target-fw/*.ihx
```

## `clockout` — the IC clock for SC-003

The programmer calibrates the IHRC (the free-pdk `EASY_PDK_CALIBRATE_IHRC` stub, VDD 4.0 V), then the IC toggles
**PA4** every 4 instruction cycles:

```
loop: xor.io pa, a    ; 1 cycle
      nop             ; 1 cycle
      goto loop       ; 2 cycles  → half-period 4, period 8 cycles
```

| File | SYSCLK | On PA4 (SYSCLK / 8) | Tolerance for SC-003 (± 1 %) |
|---|---|---|---|
| `clockout_8mhz_pfs154.ihx` | 8 MHz | **1.000 MHz** square wave | 990 … 1010 kHz |
| `clockout_1mhz_pfs154.ihx` | 1 MHz | **125.0 kHz** | 123.75 … 126.25 kHz |
| `clockout_ilrc_pfs154.ihx` | the ILRC, calibrated to 55 kHz | **6.875 kHz** | see below |

```sh
pdkprog -n PFS154 write build/target-fw/clockout_8mhz_pfs154.ihx
pdkprog start -r 4.0          # VDD 4.0 V (the calibration voltage) from the programmer; or power the target board itself
```

Measure PA4 (pin 6 of J1, `NEXT`) against GND with a frequency counter or an oscilloscope. The programmer reports
the frequency it measured during calibration ("Calibration IHRC: … measured … Hz"), so compare the instrument with
that as well. Notes:

- Measure at the calibration voltage (4.0 V): the IHRC drifts by a fraction of a percent with VDD.
- A different pin: `-DOUT_PIN=n` (a PA bit) in `build.py`. A different voltage: `-DTARGET_VDD_MV=…`.
- If the counter shows exactly ¾ or 1½ of the expected value, the loop timing assumption is wrong, not the
  calibration.
- A buzzer or other load on PA4 of the target board can distort the edges; the frequency stays the same.
- ILRC: the trim steps are 0x10 apart (free-pdk), so the result is only as close as the nearest step: compare the
  instrument with the frequency `pdkprog` reports (within 1 %), not with 55 kHz.

## `bandgap` — the BandGap reference

The programmer calibrates the IHRC (8 MHz) and the BandGap (`EASY_PDK_CALIBRATE_BG`, at VDD 3.95 V: BGTR is the first
value at which BG ≥ VDD · 12 / 40 ≈ 1.185 V). The program keeps the comparator as the calibration loop set it
((−) = BandGap, (+) = VDD · 12 / 40) and copies the result to **PA4**:

| VDD | PA4 |
|---|---|
| below ≈ 3.95 V | 0 |
| above ≈ 3.95 V | 1 (≈ VDD) |

Find the flip voltage `V_flip` with `pdkprog start -r V` in 50 mV steps (or a lab supply on the target board) and a
voltmeter on VDD **and** PA4: `BG = V_flip · 12 / 40`. Expected: `V_flip` = 3.95 V within a few tens of mV (one BGTR step).
Use the voltmeter's VDD, not the programmer's: the first G474 board reads VDD several percent off.

## `uart` — the RUN bridge

IHRC 8 MHz; after ~0.3 s the IC sends `U` (the programmer's auto-baud character) and
`PFS154 UART test: type, it echoes`, then echoes every byte. IC TX = **PA7**, RX = **PA0**, 8N1, **19200 baud**
(cycle-counted software UART).

```sh
pdkprog -n PFS154 write build/target-fw/uart_pfs154.ihx
pdkprog start -r 4.0     # expect "Connected @≈19200 baud", the greeting, then the echo of what you type; Esc stops
```
