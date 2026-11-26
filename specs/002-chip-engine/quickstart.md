# Quickstart: validating the PADAUK chip engine

Paths follow [plan.md](plan.md); the interfaces are in [contracts/](contracts/). Results of the hardware steps go into
`hw-validation.md` of this feature (constitution VI); the steps still open are listed in [hw-checklist.md](hw-checklist.md).

## Prerequisites

- The build tools of feature 001 (`software/firmware/README.md`): CMake ≥ 3.20, gcc/MinGW-w64, `arm-none-eabi-gcc` ≥ 12,
  Python ≥ 3.8, `pip install -e "software/client[dev]"`.
- Hardware: the WeAct STM32G474 board with `hardware/stm32g474-board` (supplies calibrated with `pdkprog calibrate`), a target
  board or adapter with a PFS154, a logic analyzer (≥ 8 MHz sampling on CLK/DAT/PA4), an oscilloscope (VDD, VPP), a frequency
  counter or an oscilloscope with frequency measurement. Optional: an Easy PDK programmer for SC-008.

## 1. Without hardware: traces and the IC model

```sh
python software/tests/vectors/gen_traces.py            # regenerates software/tests/vectors/traces/*.json
git diff --exit-code software/tests/vectors/traces      # the committed traces are up to date
cmake -S software/firmware -B build/host -DPLATFORM=host
cmake --build build/host
ctest --test-dir build/host --output-on-failure -R "engine"
```

Expected: `test_engine_trace` matches every family/operation of [contracts/traces.md](contracts/traces.md);
`test_engine_write` / `test_engine_otp` write and read back through the bit model, refuses a wrong ID and a missing IC without programming
voltages, keeps one session across consecutive blocks, ends every path in the safe state; `test_engine_calib` finds the trim
values on synthetic `f(k)` curves and refuses the FR-055 cases. The whole suite (`ctest`, `pytest … --sim`) stays green — the
simulator still uses its IC model.

## 2. Firmware with the engine

```sh
cmake -S software/firmware -B build/g474 -G Ninja -DPLATFORM=stm32g474 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/software/firmware/cmake/arm-cortex-m4f.cmake
cmake --build build/g474
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/g474/pdkprog-g474.bin
```

The build ends with the size check (`cmake/check_size.cmake`, ≤ 256 KB). The F072 target was removed (constitution 1.5.0).

## 3. Safety first (no IC, then an IC) — SC-005, SC-006

1. Empty socket, oscilloscope on VDD and VPP: `pdkprog probe` and `pdkprog -n PFS154 write hello.ihx` 20 times each →
   "no IC"; VPP never above 4.5 V, VDD never above 2.0 V (the probe levels).
2. A reversed PFS154 (or a resistor dummy) → same result.
3. A PFS154: start `write`, cancel it, unplug USB mid-write → VDD and VPP below 0.3 V within 10 ms of the stop.

## 4. Identify and read — Story 1, SC-004

```sh
pdkprog probe                                  # PFS154 (0xAA1), FLASH
pdkprog -n PFS154 read dump1.hex
for i in $(seq 100); do pdkprog -n PFS154 read -b d.bin; sha256sum d.bin; done | sort | uniq -c   # one hash, 100 times
```

Logic analyzer on CLK/DAT during `read`: VPP before VDD, the preamble `0xA5A5A5A6`, the 16-bit ACK ending in `0xAA1`, clock-high
≥ 1 µs.

## 5. Program — Story 2, SC-001, SC-008

```sh
time pdkprog -n PFS154 write software/tests/vectors/examples/helloworld_pfs154.ihx    # ≤ 10 s with the IHRC stub
pdkprog verify
```

Read the IC with an Easy PDK programmer and compare with `pdkprog read` (SC-008); program one with Easy PDK and read it here.

## 6. Calibrate and copy — Story 3, SC-002, SC-003

1. Program `clockout_8mhz_pfs154.ihx` (built by `software/tests/target-fw/build.py`, see its README), run the IC at
   4.0 V and measure PA4 with the frequency counter: SYSCLK / 8 = 1.000 MHz ± 1 % (SYSCLK 8 MHz ± 1 %).
2. Copy with measurement: `pdkprog copy --ihrc measure --allow-modify-master --yes`; program three other PFS154 with
   `pdkprog program`; each copy's measured frequency within 1 % of the master's; `pdkprog read` of the master before and after
   is identical.
3. ILRC and BandGap: `clockout_ilrc_pfs154.ihx` and `bandgap_pfs154.ihx` (`software/tests/target-fw`), both calibrated
   and finished; the BandGap flip voltage near 3.95 V.

## 7. Run — Story 5

`pdkprog start -r 4.0` with `uart_pfs154.ihx` (`software/tests/target-fw`: `U` + a greeting, then echo, 19200 baud):
`Connected @… baud`, the text, echo; Esc → VDD off.
