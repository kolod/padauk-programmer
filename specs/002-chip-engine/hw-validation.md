# Hardware validation: PADAUK chip engine

Results of the hardware steps of [quickstart.md](quickstart.md) (constitution VI). Board: WeAct STM32G474 +
`hardware/stm32g474-board` (supplies on, `PDK_G474_SUPPLIES=ON`); IC: PFS154 (the only IC on hand, spec FR-017).

## Families

| Family | Status |
|---|---|
| FLASH_1 (PFS154) | probe, read, erase, write confirmed on hardware (2026-11-26); calibration pending (US3) |
| FLASH_1 (PFS173), FLASH_2 (PFS172), OTP1_2, OTP2_1, OTP2_2, OTP3_1 | trace- and model-tested only — not confirmed on hardware (no IC on hand) |

## User story 1 — identify and read (T019)

| Step | Result | Date |
|---|---|---|
| `pdkprog --trace probe` with a PFS154 | **OK** — PFS154 (0xAA1, FLASH_1); trace: VPP 4500 → 4416 mV in 5.8 ms, VDD 2000 → 1976 mV in 4.0 ms, enter FLASH_1, raw answer 0x1AA1 (16 bits), leave | 2026-11-26 |
| `pdkprog --trace -n PFS154 read dump.hex` | **OK** — 2048 words, data CRC 0xD106B519 verified, content CRC 0x6076FBE3 (raw = normalized); probe, ≥ 10 ms pause, enter at the read levels (VPP 5500 → 5394 mV, VDD 2500 → 2465 mV), ACK 0x1AA1 matches, read ≈ 320 ms (≈ 156 µs/word) | 2026-11-26 |
| SC-004: 100 consecutive reads | **OK** — 100 identical SHA-256 hashes (`176ab3dd…569e66`) | 2026-11-26 |
| SC-005: no IC / reversed IC, scope on VDD/VPP | pending | |
| Logic-analyzer capture of the read preamble / ACK / clock widths | pending | |

## User story 2 — program a FLASH IC (T025)

| Step | Result | Date |
|---|---|---|
| `pdkprog --trace -n PFS154 write firmware.bin` (beeper, 488 words, one IHRC stub) — firmware built with `PDK_VDD_TOL_MV=300` | **OK up to CALIBRATE** — probe; ERASE (VDD 3000 → 3102 mV, VPP 9000 → 8996 mV, 2 clocks, 110 ms pause); BLANK_CHECK ≈ 214 ms; WRITE (VDD 5800 → 5809 mV, VPP 8500 → 8490 mV, 122 blocks in one session ≈ 83 ms); VERIFY ≈ 280 ms passed; stopped at CALIBRATE with `OP_UNSUPPORTED` (user story 3). Total 0.9 s | 2026-11-26 |
| SC-001 (full write with one IHRC calibration ≤ 10 s) | pending (needs user story 3) | |
| SC-008 (interoperability with Easy PDK) | pending | |

## Findings during bring-up

- **Target board with its own VDD source.** The first attempts failed with `VPP_FAULT` at the ID stage (VPP 1.45 V measured),
  then the +15 V overvoltage protection latched (supply capabilities gone until reset). Removing the 16 µF capacitor did not
  help; the target board also carries a CR2032 holder on VDD and a buzzer on PA3. Probing worked after the board was
  reworked (the exact change is to be recorded). Requirements for target boards were added to
  `software/firmware/platform/stm32g474/README.md` ("Target board requirements").
- **VPP about 2 % low.** VPP reaches 4416/4500 and 5394/5500 mV — at the edge of the ±2 % tolerance. Write needs VPP 8.5 V
  (±170 mV): recalibrate the supplies (`pdkprog calibrate`, target disconnected) before user story 2.
- **VDD measurement offset** (the deferred issue of the first board, PA0): with VDD off and VPP 4.5 V the VDD reading is
  0.227 V, so `pdkprog supply 0 4.5` fails the 0 V check of §6.1 by design. Not an engine issue; re-check on the second board.
- **Erase blocked by the VDD reading (2026-11-26).** The first `write` stopped before the first erase clock: VDD 3000 mV was
  read as 3100 mV (the deviation even changes sign: 2000 → 1970, 2500 → 2464, 3000 → 3100 mV) → `VDD_FAULT`. The user
  chose a temporary board workaround: the firmware for this board is built with `-DPDK_VDD_TOL_MV=300` (VDD settle check
  ±300 mV instead of ±2 %); the fault itself stays open for the second board.
- **Diagnostics.** `pdkprog --trace` (protocol 1.1) was added during bring-up: every supply step with its measured value,
  enter/leave, raw IC answers.
