# Contract: `hal_icp.h` — the IC programming lines and the calibration counter

Implemented by the G474 platform (`platform/stm32g474/hal_g474_icp.c`) and by the fake for tests
(`tests/fakes/engine/fake_icp.c`); another board would provide its own.
Used only by the engine library `core/engine/` (research R1, R2). Supplies are **not** part of this interface: the engine
sets them through `core/power` (`power_apply`), so voltage confirmation stays in one place (feature 001 FR-040).

All durations are **minimums**: an implementation may make a level longer (an interrupt, a slow edge), never shorter.

## Lines

| Name | IC pin | G474 pin | Use |
|---|---|---|---|---|
| `ICP_CLK` | PA3 | PB3 (+PA15 capture, input only) | programming clock; IC output during calibration |
| `ICP_DAT` | PA6 | PB5 | FLASH data both ways; OTP data from the IC |
| `ICP_PA4` | PA4 | PC11 | OTP data into the IC; "+1" pulses during calibration |
| `ICP_PA0` | PA0 | PB6 | OTP3_1 second clock; BandGap comparator output (input) |
| `ICP_PA7` | PA7 | PB7 | OTP3_1 "commit" |

## Functions

| Function | Behaviour |
|---|---|
| `void hal_icp_idle(void)` | all lines inputs, `ICP_CLK` and `ICP_DAT` with pull-down (the free-pdk idle state); stops any calibration peripheral. Called by the engine's safe state. Must be safe to call at any time. |
| `void hal_icp_mode(icp_line_t line, icp_dir_t dir)` | `ICP_OUT_LOW`, `ICP_OUT_HIGH`, `ICP_IN`, `ICP_IN_PULLDOWN`, `ICP_IN_PULLUP` for one line (the two pulls tell a line the IC drives from a floating one — the BandGap PA0 check) |
| `void hal_icp_clock(uint32_t high_ns)` | one pulse on `ICP_CLK`: high for ≥ `high_ns`, then low (the free-pdk "extra clock" uses 1000 ns) |
| `void hal_icp_send(icp_line_t data, icp_line_t clk, uint32_t value, uint8_t bits)` | MSB first: for each bit set `data`, `clk` high ≥ 1 µs, `clk` low; `bits` ≤ 32 |
| `uint32_t hal_icp_recv(icp_line_t data, icp_line_t clk, uint8_t bits)` | MSB first: `clk` high, ≥ 1 µs, sample `data`, `clk` low; returns the last ≤ 32 bits received |
| `bool hal_icp_level(icp_line_t line)` | read one line |
| `void hal_icp_delay_ns(uint32_t ns)` | busy wait ≥ `ns` (the DWT cycle counter on the G474) |
| `uint16_t hal_icp_vdd_max_mV(void)` | the highest IC VDD at which the IC may drive the lines the programmer reads (G474: 5000 because of PB6 FT_c; 0 = no limit); the engine refuses calibration and run above it (research R10) |

### Calibration counter

| Function | Behaviour |
|---|---|
| `bool hal_icp_calib_begin(void)` | `ICP_CLK` becomes the IC's output into the counter/SPI slave clock, `ICP_PA4` the "+1" output (low), the counter is reset; false if the peripheral cannot start |
| `bool hal_icp_calib_pulses(uint16_t n, uint32_t timeout_us)` | emit exactly `n` one-bits on `ICP_PA4`, one per IC loop turn (synchronous with `ICP_CLK` edges); returns false if the IC did not clock them out within `timeout_us` (no loop running) |
| `uint32_t hal_icp_calib_count(uint32_t window_us, uint32_t *elapsed_us)` | count `ICP_CLK` rising edges for ≥ `window_us` (≤ 100 ms per call), return the count and the measured window |
| `void hal_icp_calib_end(void)` | stop the counter and the pulse output; lines back to idle |

## Guarantees and rules

1. `hal_icp_idle()` leaves no line driven; the engine calls it **before** removing the supplies when leaving a session and
   in every safe state.
2. No function may block for more than 100 ms (the engine splits longer windows and waits).
3. The HAL never drives `ICP_CLK` while the calibration counter is active, and on the G474 never drives PA15 at all (it is
   wired to the same IC line through its own resistor).
4. Interrupts may stretch levels; nothing in the HAL depends on a maximum level duration.
5. The fake implementation records every call as an event (`mode`, `clock`, `send`, `recv`, `delay`, `calib_*`, and the
   `power_apply` calls through the fake power HAL), which is the trace format of [traces.md](traces.md).
