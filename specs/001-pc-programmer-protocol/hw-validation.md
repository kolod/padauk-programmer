# Hardware validation (T129)

An acceptance-test protocol for a board with the stub engine: programming real ICs is feature
`002-chip-engine`; here the protocol, storage, power, and standalone operation are verified. It is filled in by hand;
an empty cell means the item has not been verified yet.

**Board:** ☐ Easy PDK (STM32F072) ☐ WeAct STM32G474 + `hardware/stm32g474-board`  
**Firmware:** commit `________`, `MinSizeRel` build, `PDK_FEATURE_*` options = `________`  
**Date / who:** `________`

## 1. Firmware

| Item | Expectation | Result |
|---|---|---|
| `.text + .data` size (build output) | F072 ≤ 44 KB; G474 ≤ 256 KB | |
| `pdkprog info`: capabilities | match the build options (bits 6 `PATCH`, 9 `MEASURE_MASTER`) | |

## 2. Power safety (principle III)

| Item | Expectation | Result |
|---|---|---|
| VPP / VDD with the DAC at maximum (`pdkprog supply` with `--no-check`, multimeter) | not above the circuit's hardware limit | |
| VPP / VDD while the firmware "hangs" (stopped by a debugger or an infinite loop) | not above the hardware limit | |
| Voltages right after reset and power-on | 0 V (oscilloscope), no pulses | |
| `pdkprog selftest` with a load simulating the target board | every level: error ≤ ±2 % or ±30 mV, settling < 20 ms | |
| `pdkprog info` / `list` | protocol 1.0, the IC table | |

## 3. Storage

| Item | Expectation | Result |
|---|---|---|
| `pdkprog upload …`, power off, `pdkprog pkg check` | `VALID`, the same CRC | |
| 20 manual USB / power pulls during `upload` | after each, the package is `VALID` (old or new) | |
| Upload speed for 8 KB (`pdkprog upload`, time) | ≤ 3 s (SC-003) | |

## 4. Standalone operation

| Item | Expectation | Result |
|---|---|---|
| Local start with a corrupted package (`pdkprog pkg delete` or a damaged slot) | refusal, indication "no package" / "storage" | |
| Local start with a faulty / disconnected VPP | a self-test instead of programming, a power-fault indication | |

## 5. Serial-number entropy (R18)

| Item | Expectation | Result |
|---|---|---|
| ≥ 10⁵ generated numbers (a test build or a script) | no repeats | |
| Bit proportion test | within NIST SP 800-90B (simplified) | |

## 6. The `RUN` UART bridge (if an IC with a program that sends `'U'` is available)

| Item | Expectation | Result |
|---|---|---|
| `pdkprog start -r 3.3` | `Connected @… baud`, text from the IC; the keyboard reaches it | |
| Esc / pull USB | the voltage is removed immediately | |

## Conclusion

`________`
