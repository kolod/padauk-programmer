# Hardware

Schematic and board for the analog part of the programmer for the WeAct STM32G474CoreBoard (DipTrace):
`stm32g474-board.dchxml` is the schematic, `stm32g474-board.dipxml` is the board. MCU pin assignments,
the +15 V / −2.5 V rails, and the ICP connector are described in `software/firmware/platform/stm32g474/README.md`.

## Capacitors

| Ref. | Value | Package | Net | Working voltage | Rated voltage | Dielectric | Note |
|---|---|---|---|---|---|---|---|
| C1 | 100 nF | 0805 | +15 V ↔ −2.5 V (U1 decoupling) | up to 18.5 V | 50 V | X7R | Sits between the two rails, not to ground; 25 V is the minimum. Right at pins 8 and 4 of U1 |
| C2 | 2 × 4.7 µF | 0805 | +15 V, booster output | 15 V (up to ~17 V under D3) | 50 V (min. 35 V) | X7R | At 15 V each keeps ~1.5–2 µF, ~3–4 µF in total — enough for a load of tens of mA (ripple ≈ 40 mA · 0.67 / (200 kHz · 3 µF) ≈ 45 mV; recalculate for a different PWM frequency). Capacitors rated 16–25 V are not suitable. Low ESR: pulse current of diode D1; short loop D1 → C2 → source of Q1 |
| C11 | 2 × 4.7 µF | 0805 | +5 V, input of inductor L1 | 5 V | 50 V (16 V is enough) | X7R | 50 V so that it is the same part as C2 and C5. Ripple current of the inductor; close to L1, short loop to the source of Q1 |
| C3 | 1 µF | 0805 | PB4 → D2, flying capacitor of the −2.5 V charge pump | ≤ 3.3 V | 16 V | X7R | Low ESR |
| C5 | 4.7 µF | 0805 | −2.5 V, reservoir | ~2.7 V | 50 V (10 V is enough) | X7R | Non-polarized — the polarity of the negative rail does not matter |
| C4 | 100 nF | 0805 | −2.5 V, HF decoupling | ~2.7 V | 16–50 V | X7R | Close to pin 4 of U1 |
| C6 | 100 nF | 0805 | ADC_Vdd (PA0), divider filter | ≤ 1.1 V | 16–50 V | X7R | τ = (100k ∥ 20k) · 100 nF ≈ 1.7 ms |
| C7 | 100 nF | 0805 | ADC_Vpp (PA1), divider filter | ≤ 2.3 V | 16–50 V | X7R | τ ≈ 1.7 ms |
| C8 | 1 nF | 0805 | COMP_OVP (PB1), OVP divider | ≤ 1.2 V | 50 V | C0G/NP0 | τ = (130k ∥ 10k) · 1 nF ≈ 9 µs — filters noise without slowing the OVP |
| C9 | 10 nF | 0805 | DAC_Vdd (PA5) → U1 input | ≤ 3.3 V | 50 V | C0G/NP0 | See "DAC outputs" |
| C10 | 10 nF | 0805 | DAC_Vpp (PA4) → U1 input | ≤ 3.3 V | 50 V | C0G/NP0 | See "DAC outputs" |

### Purchasing summary

The same capacitors, grouped by value. The voltage rating is raised where that removes an extra
part number (C4/C6/C7 — 50 V like C1; C5, C11 — 50 V like C2), and C2 and C11 are built from two
4.7 µF 0805 capacitors instead of one 10 µF 1206: 5 line items for 13 capacitors. On the schematic,
C2 and C11 must each be split into two capacitors (add two new positions).

| Value | Package | Voltage | Dielectric | Quantity | Refs |
|---|---|---|---|---|---|
| 1 nF | 0805 | 50 V | C0G/NP0 | 1 | C8 |
| 10 nF | 0805 | 50 V | C0G/NP0 | 2 | C9, C10 |
| 100 nF | 0805 | 50 V | X7R | 4 | C1, C4, C6, C7 |
| 1 µF | 0805 | 25 V | X7R | 1 | C3 |
| 4.7 µF | 0805 | 50 V | X7R | 5 | C2 (2 pcs), C11 (2 pcs), C5 |

Example for 4.7 µF: CERCAP 4.7/50V 0805 KX7R (X7R, ±10 %).

General rule: 0805 X7R, and C0G for small values (1–10 nF); the rated voltage of a ceramic
capacitor is at least twice the working voltage (50 V for C1 and C2), because the capacitance of
X7R/X5R drops under DC bias.

C8, C9, and C10 sit in reference-voltage and threshold circuits, hence C0G: the capacitance does
not depend on voltage or temperature, and there is no piezoelectric effect or dielectric
absorption. At 1–10 nF in 0805 / 50 V, C0G costs the same as X7R. For C6 and C7 the type is not
critical: the voltage is DC, and accuracy is set by calibration.

### DAC outputs

The DAC works without a buffer, and its output resistance is ≈ 15 kΩ:

- with 10 nF at C9/C10, τ ≈ 150 µs and settling to 0.1 % takes ≈ 1 ms; after changing the DAC code
  the firmware must wait ~1–2 ms before measuring;
- R1/R6 (240k to ground) together with the 15 kΩ of the DAC form a divider (≈ −6 % of the voltage) —
  this error is removed by the supply calibration.

### Vdd / Vpp outputs

There are no capacitors on the outputs after R17/R18. If needed, up to 100 nF X7R / 50 V can be
added at J1.1 and J1.5 next to the connector: R17/R18 (20 Ω) isolate the capacitance from the
op-amp output, so there will be no oscillation. The price is a VPP edge of τ = 20 Ω · 100 nF = 2 µs
and a larger inrush current at switch-on; do not exceed ~100 nF.
