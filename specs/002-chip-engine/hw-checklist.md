# Hardware checklist: what is left to check (feature 002)

The open hardware steps of [tasks.md](tasks.md) (T019, T025, T030, T037) as a bench list. Commands and expected results
follow [quickstart.md](quickstart.md); the test programs are in
[`software/tests/target-fw`](../../software/tests/target-fw/README.md). Write every result (value, instrument, date) into
[hw-validation.md](hw-validation.md); when the list is done, finish it as the final record (T045).

## 0. Preparation

- [ ] Build the test programs: `python software/tests/target-fw/build.py` → `build/target-fw/*.ihx`.
- [ ] Programmer firmware from `main` flashed (`build/g474`, still with `PDK_VDD_TOL_MV=300` on board #1).
- [ ] Supplies calibrated: `pdkprog calibrate` with the target disconnected.
- [ ] Instruments: frequency counter or oscilloscope with frequency measurement, voltmeter, oscilloscope with 2 channels
      (VDD, VPP), logic analyzer ≥ 8 MHz.
- [ ] Probe points on J1: 1 VDD, 2 VPP (PA5), 3 PA6/ICPDA, 4 PA7, 5 PA3/ICPCK, 6 PA4, 7 PA0, 8 GND.

## 1. IHRC clock — SC-003 (T030)

- [ ] `pdkprog -n PFS154 write build/target-fw/clockout_8mhz_pfs154.ihx`; note the reported "measured … Hz".
- [ ] `pdkprog start -r 4.0`; counter on PA4 (J1-6): **1.000 MHz ± 1 %** (990…1010 kHz) = SYSCLK 8 MHz ± 1 %.
- [ ] The counter agrees with the frequency `pdkprog` reported / 8 to within ~0.2 %.
- [ ] Optional: `clockout_1mhz_pfs154.ihx` → 125.0 kHz ± 1 %.
- [ ] If the reading is exactly ¾ or 1½ of the expected value: the loop cycle count is wrong (report it), not the
      calibration.

## 2. ILRC (T030)

- [ ] `pdkprog -n PFS154 write build/target-fw/clockout_ilrc_pfs154.ihx`: "Calibration ILRC: target 55000 Hz, value 0x?0 …
      [OK]", note the measured frequency.
- [ ] `pdkprog start -r 4.0`; counter on PA4 ≈ **6.875 kHz**; within 1 % of (reported frequency / 8). The value is a
      multiple of 0x10 (coarse steps), so the distance from 55 kHz may be several percent — that is expected.

## 3. BandGap (T030)

- [ ] `pdkprog -n PFS154 write build/target-fw/bandgap_pfs154.ihx`: two calibrations, IHRC 8 MHz and "BG … [OK]".
- [ ] `pdkprog start -r V` for V = 3.80, 3.85 … 4.10 V; at each step measure VDD (J1-1) **with the voltmeter** and the
      level on PA4 (J1-6).
- [ ] PA4 = 0 below and 1 above the flip voltage `V_flip`; **`V_flip` ≈ 3.95 V** (within one BGTR step, a few tens of mV).
      BandGap = `V_flip · 12 / 40` ≈ 1.185 V.

## 4. RUN bridge (T037)

- [ ] `pdkprog -n PFS154 write build/target-fw/uart_pfs154.ihx`.
- [ ] `pdkprog start -r 4.0` → `Connected @≈19200 baud`, then `PFS154 UART test: type, it echoes`.
- [ ] Typed characters come back (echo), including a burst (paste a line).
- [ ] Esc → VDD at J1-1 below 0.3 V within 10 ms (scope).
- [ ] Start again, unplug USB → VDD below 0.3 V within 10 ms (scope).
- [ ] `pdkprog start -r 5.5` is refused (the 5.0 V pin limit), no voltage on J1-1.

## 5. Safety — SC-005, SC-006 (T019)

Scope on VDD (J1-1) and VPP (J1-2), single-shot trigger on VPP rising.

- [ ] Empty socket: `pdkprog probe` × 20 → "no IC"; VPP never above **4.5 V**, VDD never above **2.0 V**.
- [ ] Empty socket: `pdkprog -n PFS154 write build/target-fw/clockout_8mhz_pfs154.ihx` × 20 → "no IC"; same limits.
- [ ] Reversed IC — **better a resistor dummy** (e.g. 1 kΩ VDD–GND, 10 kΩ on the lines) to spare the PFS154: probe ×
      20 and write × 20 → no IC / wrong ID, same limits.
- [ ] SC-006: cancel a `write` (Ctrl+C) during WRITE and during ERASE → VDD and VPP below 0.3 V within 10 ms.
- [ ] SC-006: unplug USB during WRITE → VDD and VPP below 0.3 V within 10 ms; afterwards the IC can be programmed again.

## 6. Logic-analyzer captures (T019, T025)

Channels: PA3/ICPCK (J1-5), PA6/ICPDA (J1-3), PA4 (J1-6); optionally VDD/VPP on the scope.

- [ ] `read`: VPP before VDD, the preamble `0xA5A5A5A6`, a 16-bit ACK ending in `0xAA1`, clock high ≥ 1 µs, CLK high ≥ 5 µs
      before the first data bit of every word.
- [ ] One write block (PFS154): 4 × 14-bit data, 13-bit address, then 8 clocks of 15 µs high / 15 µs low, one extra clock,
      25 µs; 100 µs between blocks.
- [ ] One erase: 2 clocks with 5 ms high, low 1 µs / high 1 µs / low 4 µs, one extra clock; VDD 3.0 V, VPP 9.0 V.
- [ ] Save the captures (e.g. `.sr` files) and reference them in hw-validation.md.

## 7. Optional — SC-001 with a full image

- [ ] A full-size PFS154 image with an IHRC stub **and a fuse word**: `time pdkprog -n PFS154 write …` ≤ 10 s
      (recorded so far: 4.6 s with a 488-word image without a fuse).

## 8. Needs more hardware

- [ ] SC-002 (needs 3 more PFS154): program a master with `clockout_8mhz_pfs154.ihx`;
      `pdkprog copy --ihrc measure --allow-modify-master --yes`; program three copies; each copy's measured frequency
      (and on the counter, as in §1) within 1 % of the master's; `pdkprog read` of the master before and after identical.
- [ ] SC-008 (needs an Easy PDK programmer): an IC programmed here reads back identically on Easy PDK, and one
      programmed by Easy PDK reads back identically here.
- [ ] Board #2: re-check the VDD measurement (the PA0 wander of board #1); if it is fine, rebuild without
      `PDK_VDD_TOL_MV` and repeat §1 and one `write`.
