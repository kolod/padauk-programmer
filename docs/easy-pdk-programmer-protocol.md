# Easy PDK Programmer — the PC ⇄ programmer ⇄ PADAUK IC protocols

This document is based on an analysis of the `easy-pdk-programmer-software` sources
(the `easypdkprog` host utility v1.3, STM32F072 firmware SW 1.3 / HW 1.2, protocol **1.3**)
and the `easy-pdk-programmer-hardware` schematic (rev 1.2).

The goal is to give enough information to implement a compatible programmer on other hardware
(a different MCU, different DACs/voltage sources) so that the **original** `easypdkprog` utility works with it,
or to write your own host program.

Code references (for checking details):

| What | File |
|---|---|
| Command / response / error codes | [Firmware/source/Src/fpdkproto.h](../easy-pdk-programmer-software/Firmware/source/Src/fpdkproto.h) |
| USB packet handling in the firmware | [Firmware/source/Src/fpdkusb.c](../easy-pdk-programmer-software/Firmware/source/Src/fpdkusb.c) |
| Low-level protocol to the PADAUK IC | [Firmware/source/Src/fpdk.c](../easy-pdk-programmer-software/Firmware/source/Src/fpdk.c) |
| UART bridge for run mode | [Firmware/source/Src/fpdkuart.c](../easy-pdk-programmer-software/Firmware/source/Src/fpdkuart.c) |
| Host: packet construction | [fpdkcom.c](../easy-pdk-programmer-software/fpdkcom.c) |
| Host: write/read algorithm | [easypdkprog.c](../easy-pdk-programmer-software/easypdkprog.c) |
| Host: IC table | [fpdkicdata.c](../easy-pdk-programmer-software/fpdkicdata.c) |
| Host: calibration | [fpdkiccalib.c](../easy-pdk-programmer-software/fpdkiccalib.c) |
| Host: serial-number stub | [fpdkicserial.c](../easy-pdk-programmer-software/fpdkicserial.c) |

---

## 1. Architecture

```
 ┌──────────┐  USB CDC (virtual COM)       ┌──────────────────┐   PA3/PA4/PA5/PA6/PA0/PA7, VDD, GND  ┌────────────┐
 │    PC    │ ───────────────────────────► │    Programmer    │ ───────────────────────────────────► │  PADAUK IC │
 │easypdkprog│ ◄─────────────────────────── │ (STM32F072 + DAC │ ◄─────────────────────────────────── │ PFS/PMS/.. │
 └──────────┘   "cmd,len,payload" packets  │  + amplifiers)   │   bit-bang serial protocol           └────────────┘
                                           └──────────────────┘
```

Division of responsibility:

* The **PC** knows everything about the specific IC (the parameter table: word width, voltages, block timings),
  parses the HEX, inserts calibration stubs and the serial number, and drives the sequence
  "erase → blank check → write → verify → calibrate → write fuse".
* The **programmer** is a "dumb" executor: it stores a 4096-word buffer, can generate VDD/VPP at the given
  values in millivolts, and perform primitive operations (probe / blank check / erase / read / write /
  verify / calibrate / run). All operation parameters come from the PC in every command.
* The **IC** is programmed over 4 lines: VDD, GND, PA5 (VPP — the high voltage), PA3 (clock),
  PA6 and/or PA4 (data). Some families and modes additionally use PA0 and PA7.

---

## 2. PC ⇄ programmer transport

### 2.1 USB

* USB CDC ACM class (a virtual COM port). The baud rate is ignored; the host sets 115200 8N1 with no
  flow control.
* VID/PID: `0x0483` / `0x5740` (the standard STM32 Virtual COM Port), Manufacturer `free-pdk.github.io`,
  Product `Easy PDK Programmer`, Serial `1234567855AA` (on macOS the host looks for exactly
  `/dev/tty.usbmodem1234567855AA*`; on Linux — `/dev/ttyACM*`, on Windows — `COM1..COM999`).
* **Mandatory:** the firmware processes commands only when the host has set **DTR**
  (`SET_CONTROL_LINE_STATE`, bit 0). Every change of the line state (opening/closing the port):
  * resets the packet receive buffer;
  * fills the IC data buffer with `0xFF`;
  * if the IC was started with the `X` command — turns off the UART and VDD.

### 2.2 Packet format

All multi-byte fields are **little-endian**. Voltages are `uint32` in **millivolts**.

**PC → programmer (command):**

| Byte | Field | Description |
|---|---|---|
| 0 | `cmd` | the ASCII code of the command (see 2.4) |
| 1 | `len` | payload length, 0…255 |
| 2… | `payload` | `len` bytes |

**Programmer → PC (response):**

| Byte | Field | Description |
|---|---|---|
| 0 | `rsp` | `'A'` (0x41) — ACK, `'E'` (0x45) — error, `'D'` (0x44) — data from the IC's UART |
| 1–2 | `len` | `uint16` payload length |
| 3… | `payload` | `len` bytes |

Rules:

* The programmer accumulates bytes until `2 + len` have arrived, then executes the command (synchronously, blocking)
  and shifts the buffer. The receive buffer size is 258 bytes.
* An `'E'` response always has `len = 0`. It is sent if: the command is unknown, the payload is shorter than
  required, a buffer limit is exceeded, calibration failed, or voltage setting failed in `X`.
* **IC operation errors** are returned not through `'E'` but as `'A'` with a 16-bit result code
  (see 2.3).
* `'D'` packets are sent asynchronously (only in the IC run mode) and can appear between responses.
* Before the first command the host reads and discards everything in the port.

### 2.3 IC operation result (16 bit)

The commands `Z`, `E`, `R`, `W`, `V` return a `uint16`:

| Value | Name | Meaning |
|---|---|---|
| `== icid` | — | success (the same 12-bit ID that was passed is returned) |
| `0xFFFF` | `FPDK_ERR_UKNOWN` | unknown error / invalid parameters (e.g. `write_block_size` 0 or >8, erase for OTP) |
| `0xFFFE` | `FPDK_ERR_VPPVDD` | could not apply VPP/VDD to enter programming mode |
| `0xFFFD` | `FPDK_ERR_HVPPHVDD` | could not apply the "working" (high) VPP/VDD |
| `0xFFFC` | `FPDK_ERR_CMDRSP` | the ID in the IC's response did not match the expected one |
| `0xFFFB` | `FPDK_ERR_VERIFY` | verification mismatch |
| `0xFFFA` | `FPDK_ERR_NOTBLANK` | the IC is not blank |

The host treats any value `>= 0xFFF0` as an error and takes the text by index `value & 0x0F`.

### 2.4 Commands

The timeouts are how long the host waits for a response; **a compatible implementation must fit within them**.

| Cmd | Name | Request payload | `'A'` response | Host timeout |
|---|---|---|---|---|
| `I` | GETVERINFO | — | an ASCII version string | 50 ms |
| `L` | SETLED | `u8 mask` | — | 50 ms |
| `B` | GETBUTTON | — | `u8 pressed` | 50 ms |
| `O` | SETVOLTOUT | `u32 vdd_mV, u32 vpp_mV` | — | 50 ms |
| `U` | GETVOLTAGES | — | `u32 vref_mV, u32 vdd_mV, u32 vpp_mV` | 50 ms |
| `S` | SETBUF | `u16 byte_offs, u8 data[]` | — | 50 ms |
| `G` | GETBUF | `u16 byte_offs, u16 len` | `data[len]` | 50 ms |
| `P` | PROBEIC | — | `u32 rsp, u32 vpp_mV, u32 vdd_mV, u32 type` | 3500 ms |
| `Z` | BLANKCKIC | 28 bytes, see below | `u16 result` | 1000 ms |
| `E` | ERASEIC | 20 bytes | `u16 result` | 2000 ms |
| `R` | READIC | 27 bytes | `u16 result` | 1000 ms |
| `W` | WRITEIC | 30 bytes | `u16 result` | **1000 ms** |
| `V` | VERIFYIC | 32 bytes | `u16 result` | 1000 ms |
| `C` | CALIBRATEIC | 16 bytes | `u32 value, u32 freq_Hz` | 10000 ms |
| `X` | EXECUTEIC | `u32 vdd_mV` | — | 50 ms |
| `Q` | STOPIC | — | — | 50 ms |
| `D` | DBGDAT | `u8 data[]` | **no response** | — |

#### `I` — version

The response is a string with no trailing zero:

```
FREE-PDK EASY PROG - HW:1.2 SW:1.3 PROTO:1.3\n
```

The host parses it with `sscanf("FREE-PDK EASY PROG - HW:%u.%u SW:%u.%u PROTO:%u.%u\n")` and **requires an exact
match of PROTO 1.3**, otherwise "Programmer protocol mismatch". HW/SW can be anything.

#### `L` — LEDs

`mask`: bit0 — LED1, bit1 — LED2, bit2 — LED3. (The firmware uses LED1 as the UART receive indicator,
LED2 — UART transmit, LED3 — "an IC operation is in progress".)

#### `B` — button

`1` if the button has been pressed stably for > 10 ms, otherwise `0`.

#### `O` / `U` — manual voltage control

`O` sets VDD and VPP (turns on all LEDs). `U` returns the ADC-measured values:
the MCU's supply reference voltage, VDD, and VPP (mV). `easypdkprog` does not use these commands; they are for
testing (`easypdkprogtest`).

#### `S` / `G` — the data buffer

The programmer buffer is `uint16 buf[0x1000]` (8192 bytes), words little-endian, not initialized — after
the port is opened it is filled with `0xFFFF`.

* `S`: `byte_offs` is an offset **in bytes**, followed by the data (the host sends chunks of 252 bytes).
* `G`: reads `len` bytes starting at `byte_offs`. The response may be longer than 255 bytes (the length field
  is 16-bit), but the host on Windows asks for 63 bytes at a time.
* Out of bounds → `'E'`.

#### `P` — IC search

The programmer tries the families in turn (see 4.6) at VPP = 4.5 V, VDD = 2.0 V.

| Field | Description |
|---|---|
| `rsp` | the raw IC ID response (`& 0x7FFFFFFF`); `0` — nothing found |
| `vpp_mV`, `vdd_mV` | the voltages at which it was found (4500 / 2000) |
| `type` | the protocol type: `0xF0` (any FLASH), `0x12`, `0x22`, `0x21`, `0x31` |

How the host turns `rsp` into a 12-bit ID is in 4.6.

#### The common header of the commands `Z`, `E`, `R`, `W`, `V`

| Offset | Type | Field |
|---|---|---|
| 0 | `u16` | `icid` — the expected 12-bit ID |
| 2 | `u8` | `type` — the protocol type (`0x12`, `0x21`, `0x22`, `0x31`, `0xF1`, `0xF2`) |
| 3 | `u32` | `vdd_cmd` — VDD while entering the mode and sending the command |
| 7 | `u32` | `vpp_cmd` — VPP while entering the mode and sending the command |
| 11 | `u32` | `vdd_hv` — VDD during the operation (read/erase/write) |
| 15 | `u32` | `vpp_hv` — VPP during the operation |

> ⚠️ A host quirk: in `Z`, `R`, `V` the host fills byte 12 (the second byte of `vdd_read`) with
> `vdd_cmd >> 8`. On all known ICs `vdd_cmd == vdd_read`, so this does not show up, but
> a compatible firmware should simply take the bytes as they are.

#### `Z` — blank check (28 bytes)

| Offset | Type | Field |
|---|---|---|
| 0–18 | | the common header (`vdd_hv/vpp_hv` = the read voltages) |
| 19 | `u8` | `addr_bits` |
| 20 | `u8` | `data_bits` |
| 21 | `u16` | `count` — how many words to check, starting at address 0 |
| 23 | `u8` | `excl_first` — skip address 0 |
| 24 | `u16` | `excl_start` |
| 26 | `u16` | `excl_end` |

Addresses `p` = 0…count-1 are checked, except `p == 0` (if `excl_first`) and except
`excl_start <= p <= excl_end` (**both bounds inclusive**). A blank word = all `data_bits` ones.

#### `E` — erase (20 bytes, FLASH only)

| Offset | Type | Field |
|---|---|---|
| 0–18 | | the common header (`vdd_hv/vpp_hv` = the erase voltages) |
| 19 | `u8` | `erase_clocks` |

#### `R` — read (27 bytes)

| Offset | Type | Field |
|---|---|---|
| 0–18 | | the common header (`vdd_hv/vpp_hv` = the read voltages) |
| 19 | `u16` | `addr` — the start address in the IC (in words) |
| 21 | `u8` | `addr_bits` |
| 22 | `u16` | `data_offs` — the start index in the programmer buffer (**in words**) |
| 24 | `u8` | `data_bits` |
| 25 | `u16` | `count` — the number of words |

The words read are put into `buf[data_offs + i]`, and the host then fetches them through `G`.

#### `W` — write (30 bytes)

Like `R` (voltages 11/15 are the write ones), plus:

| Offset | Type | Field |
|---|---|---|
| 27 | `u8` | `block_size` — words per write cycle (1…8) |
| 28 | `u8` | `clock_groups` — the number of groups of programming pulses |
| 29 | `u8` | `clocks_per_group` — pulses in a group |

The data is taken from `buf[data_offs…]`.

#### `V` — verify (32 bytes)

Like `R`, plus:

| Offset | Type | Field |
|---|---|---|
| 27 | `u8` | `excl_first` — skip IC address 0 (`addr + p == 0`) |
| 28 | `u16` | `excl_start` (relative to `p`, inclusive) |
| 30 | `u16` | `excl_end` (inclusive) |

The low `data_bits` bits are compared. If the expected word from the buffer = all ones (`0xFFFF & mask`),
it is considered a "wildcard" and is **not checked** (these are unwritten/skipped words).

#### `C` — calibration (16 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | `type`: 1 — IHRC, 2 — ILRC, 3 — BandGap |
| 4 | `u32` | `vdd_mV` — the IC supply voltage during calibration |
| 8 | `u32` | `freq_Hz` — the target SYSCLK frequency |
| 12 | `u32` | `mult` — CPU clocks per signal period (= 7) |

The response: `u32 value` (the trim register value, only the low byte is used) and
`u32 freq_Hz` — the frequency reached (0 for BG). Details are in section 6.

#### `X` / `Q` / `D` — running the program in the IC

* `X`: apply VDD = `vdd_mV` to the IC (VPP = 0), turn on the UART bridge with baud-rate auto-detection. ACK.
* `Q`: turn off the UART, VDD = 0, turn off the LEDs. ACK.
* `D` (PC → programmer): the bytes are sent to the IC over UART. No response (so as not to mix with the incoming stream).
* `D` (programmer → PC): the bytes received from the IC.

Details are in section 7.

---

## 3. The programmer hardware

### 3.1 Connection to the IC

| IC signal | Direction | STM32 pin (original) | Purpose |
|---|---|---|---|
| **VDD** | output | DAC1 (PA4) → op-amp ×2 | IC supply, 0…6.26 V, programmable |
| **GND** | — | — | common |
| **PA5** | output | DAC2 (PA5) → op-amp ×4.23 | **VPP**, 0…13.27 V, programmable |
| **PA3** | output / input | PB3 (GPIO / SPI1_SCK) | protocol CLK; during calibration — the IC's output |
| **PA6** | bidirectional | PB5 (GPIO / SPI1_MOSI) | DAT (FLASH: bidirectional; OTP: data from the IC) |
| **PA4** | output / input | PB4 (GPIO / SPI1_MISO) | data into the IC for OTP; during calibration — the IC's input |
| **PA0** | bidirectional | PB6 (GPIO / USART1_TX) | CLK2 for OTP3_1; UART → IC; the comparator output during BG calibration |
| **PA7** | bidirectional | PB7 (GPIO / USART1_RX) | "commit" for OTP3_1; UART ← IC |

All signal lines have 100 Ω series resistors, and VDD and VPP have 100 nF to ground near the socket.
When the lines are not in use, they are switched to input (DAT/PA6 and CLK/PA3 — with a pull-down to ground).

Requirements for new hardware:

* **VDD**: 0…≥6.0 V (the maximum used is 5.8 V), set with a step of ~10 mV (the original is a 12-bit
  DAC: `code = mV * 4095 / 6260`). It must settle quickly: the firmware waits only 100 µs…10 ms.
* **VPP**: 0…≥11.5 V (the maximum in the table is 11.0 V for PMS154), `code = mV * 4095 / 13270`. It must
  be able to drop to 0 V (the original has an op-amp powered from +15 V / −2.4 V; 15 V comes from an MT3608, −2.4 V from a
  diode charge pump driven by a 50 kHz PWM on the STM32's PA3).
* Both outputs are voltage sources controlled without feedback from the ADC (the ADC is only for `U`).
  The original measures VDD and VPP through 100k/20k dividers (×6) on PB1 and PB0.
* Logic levels: the STM32 runs from 3.3 V, and lines PB3…PB7 are 5V-tolerant. During programming the IC's VDD
  rises to 5.3…5.8 V, so your inputs must withstand ~6 V (or add protection/level
  shifters that do not distort the ~1 µs timings).
* Calibration needs a **hardware counter/timer clocked by an external signal** (the original uses
  SPI1 in slave mode with DMA + a 32-bit timer at 48 MHz). See section 6.
* Run mode needs a UART with baud-rate auto-detection (or a software implementation).

### 3.2 Software delays

The base delay `DelayUS(n)` in the original is a loop of `10*n` iterations at 48 MHz, that is, ≈ n µs.
A CLK pulse: `CLK=1; delay 1 µs; CLK=0`. The low level lasts only as long as the code
between pulses takes (hundreds of ns). So the actual bit rate is ≈ 0.5…1 Mbit/s. The longer timings (15/30/40 µs,
milliseconds when erasing) are **mandatory** programming pulse durations.

---

## 4. The programmer ⇄ IC protocol

### 4.1 Protocol types (families)

| Code | Name | ICs | CLK | Data → IC | Data ← IC |
|---|---|---|---|---|---|
| `0xF1` | FLASH_1 | PFS154, PFS173 | PA3 | PA6 | PA6 |
| `0xF2` | FLASH_2 | PFS172 | PA3 | PA6 | PA6 |
| `0x12` | OTP1_2 | PMS150C/PMS15A, PMS154B/C, MCU390 | PA3 | PA4 | PA6 |
| `0x21` | OTP2_1 | PMS133/PMS134 | PA3 | PA4 | PA6 |
| `0x22` | OTP2_2 | PMS131, PMS132, PMS152, PMS171B | PA3 | PA4 | PA6 |
| `0x31` | OTP3_1 | PMC251, PMS271/PMC271 (read only) | PA3 (command), PA0 (address/data) | PA4 | PA6 |
| `0xF0` | FLASH (generic) | only for `P` (probe) | PA3 | PA6 | PA6 |

VPP is always applied to **PA5**.

### 4.2 Elementary operations (bit-bang)

All sequences are transmitted **most significant bit first (MSB first)**.

**Sending a bit** (programmer → IC):

```
DATA ──X═══════ bit ═══════X═══ next bit ═══
CLK  ────┐  ┌────┐  ┌───── ...
         └──┘    └──┘           (more precisely: 0 → ↑ → 1 µs → ↓)
     ____/‾‾‾‾\____/‾‾‾‾\___
         ≥1 µs
```

1. set DATA = bit;
2. CLK = 1, wait 1 µs;
3. CLK = 0.

The data is stable the whole time CLK is high. For the OTP families, after the last bit the data line (PA4)
is forced to 0; for FLASH the PA6 line keeps the last bit.

**Receiving a bit** (IC → programmer):

1. CLK = 1, wait 1 µs;
2. read the data line (PA6, or PA6 on CLK2 for OTP3_1);
3. CLK = 0.

The result is assembled as `value = (value << 1) | bit` — the first received bit is the most significant.

**An "extra clock"** is a single CLK pulse (1 µs) with no change of data.

For OTP3_1 there are variants with CLK2 = PA0 instead of PA3 and a "commit": `PA7 = 1; 1 µs; PA7 = 0;` + one CLK2 pulse.

### 4.3 Entering and leaving programming mode

**Enter** (`Enter(type, vpp_cmd, vdd_cmd)`):

1. PA3 (CLK) → output, 0.
2. VPP = `vpp_cmd`, wait **100 µs**.
3. VDD = `vdd_cmd`, wait **500 µs**.
4. FLASH: PA6 → output, 0.
   OTP: PA6 → input (pull-down), PA4 → output, 0. For OTP3_1 also PA0 and PA7 → output, 0.

So VPP is applied **before** VDD — this is the sign of entering programming mode.

**Leave** (`Leave(extra)`):

1. PA6 → input (pull-down), PA4, PA0, PA7 → inputs.
2. VDD = 0, VPP = 0 (no delay between them).
3. Wait **10 ms + extra** (after erase/write `extra` = 100 ms). The firmware explicitly comments that
   waiting after leaving before the next command is mandatory.
4. PA3 → input (pull-down).

**Changing voltages inside the mode:**

* for reading (if `vdd_hv/vpp_hv` differ from `cmd`): VDD → wait 1 ms, VPP → wait 1 ms;
* for write/erase: VDD → wait **10 ms**, VPP → wait **10 ms**.

> For PFS172 `vpp_hv` = 0 V: during read/write/erase VPP is **removed** to zero while VDD is raised.

### 4.4 The command (preamble)

| Type | What is sent | Response |
|---|---|---|
| FLASH_1, FLASH (probe) | 32 bits `0xA5A5A5A0 \| cmd` on PA6 | PA6 → input, receive **16** bits (ACK), PA6 → output, +1 clock |
| FLASH_2 | 32 bits `0xA5A5A5A0 \| cmd` on PA6 | receive **17** bits, PA6 → output, +1 clock |
| OTP1_2 | 32 bits `0xA5A5A5A0 \| cmd` on PA4 | — |
| OTP2_1, OTP2_2 | 32 bits `0x5A5A5A50 \| cmd` on PA4 | — |
| OTP3_1 | 32 bits `0x5A5A5A5A`, then 27 zero bits on PA4 (PA3 clock); `cmd` is not sent | — |

The `cmd` codes (4 bits):

| Operation | FLASH_1 / OTP | FLASH_2 |
|---|---|---|
| Read | `0x6` | `0xC` |
| Write | `0x7` | `0x7` |
| Erase | `0x3` | `0x5` |

**ACK in FLASH:** the low 12 bits of the received word = the IC ID (`ack & 0xFFF == icid`).
If they do not match → leave the mode and `FPDK_ERR_CMDRSP`.

### 4.5 Getting the ID for OTP (`GetID`)

The OTP families give no ACK for the command. The ID is obtained in a separate pass **before each**
Read/Write/Verify/BlankCheck operation:

1. `Enter(vpp_cmd, vdd_cmd)`.
2. The **Write** command (`0x7`).
3. Receive N bits:
   * OTP1_2, OTP2_2: `N = 2*data_bits + 12`;
   * OTP2_1: `N = data_bits + 12`;
   * OTP3_1: `N = data_bits + 1 + 12`, clock **PA0** (CLK2), data on PA6.
4. `Leave()` — the write never starts (VPP/VDD are removed before the high voltages are applied).
5. `ID = value & 0xFFF` (the last 12 received bits) must equal `icid`, otherwise `FPDK_ERR_CMDRSP`.

A 32-bit variable is used for accumulation, so for N > 32 only the last 32 bits are kept.

### 4.6 Probe (auto-detection)

The programmer tries in turn, each time a full Enter/Leave at VPP = 4.5 V, VDD = 2.0 V:

1. FLASH: the Read command (`0x6`) with a 16-bit ACK; a nonzero ACK → type `0xF0`.
2. OTP1_2 with `data_bits = 16` (N = 44); if `rsp & 0xFFFF != 0` → `0x12`.
3. OTP2_2 with `data_bits = 16` → `0x22`.
4. OTP2_1 with `data_bits = 16` (N = 28) → `0x21`.
5. OTP3_1 with `data_bits = 16` (N = 29) → `0x31`.

The host turns `rsp` into a 12-bit ID as follows:

* FLASH: `id = rsp & 0xFFF`; if not found in the table — `(rsp << 1) & 0xFFF` (PFS172 gives a 17-bit ACK,
  and probe reads only 16).
* OTP: since the real word width is unknown and it was read as for 16 bits, it tries
  `codebits` = 16, 15, 14, 13 with a shift `rsp >> (k * (16 - codebits))`, where `k = 1` for OTP2_1/OTP3_1
  and `k = 2` for OTP1_2/OTP2_2. A match is looked up by (`id12`, `type`, `codebits`).

### 4.7 Reading a word

| Type | Sequence for one address |
|---|---|
| FLASH_1, FLASH_2 | `addr` (`addr_bits`) on PA6 → PA6 input → CLK = 1, **5 µs** → receive `data_bits` (the first bit is read while CLK is still high after these 5 µs + 1 µs) → PA6 output → +1 clock |
| OTP1_2, OTP2_x | `addr` on PA4 (PA3 clock) → receive `data_bits` from PA6 |
| OTP3_1 | `addr` on PA4 with the **PA0** clock → commit (a PA7 pulse of 1 µs + 1 PA0 clock) → receive `data_bits` from PA6 with the PA0 clock |

**The full READ operation:**

```
[OTP: GetID pass with an ID check]
Enter(vpp_cmd, vdd_cmd)
SendCommand(Read)                → for FLASH: ACK check
[if hv ≠ cmd: VDD=vdd_hv, 1 ms; VPP=vpp_hv, 1 ms]
for p in 0..count-1:  buf[data_offs+p] = ReadAddr(addr+p)
Leave(0)
```

BLANKCHECK and VERIFY are identical, only instead of storing there is a comparison (the BLANKCHECK addresses are always
from 0). At the first mismatch the loop is interrupted.

### 4.8 Erase (FLASH only)

```
Enter(vpp_cmd, vdd_cmd)
SendCommand(Erase: 0x3 / FLASH_2: 0x5) → ACK check
VDD = vdd_hv, 10 ms;  VPP = vpp_hv, 10 ms
repeat erase_clocks:
    CLK=1, wait 5 ms   (FLASH_2: 40 ms)
    CLK=0, 1 µs
    CLK=1, 1 µs
    CLK=0, 4 µs
+1 clock
Leave(100 ms)
```

PA6 is an output at 0 the whole time.

### 4.9 Write

**The general WRITE algorithm:**

```
[OTP: GetID pass with an ID check]
Enter(vpp_cmd, vdd_cmd)
SendCommand(Write 0x7)             → for FLASH: ACK check
VDD = vdd_hv, 10 ms;  VPP = vpp_hv, 10 ms
for p = 0; p < count; p += block_size:
    wbuf[0..block_size-1] = 0xFFFF
    copy min(block_size, count-p) words into wbuf, starting at index (addr % block_size)
    if wbuf has at least one word ≠ all ones (by data_bits):
        WriteBlock(aligned_addr = ((addr+p) / block_size) * block_size, wbuf)
    wait 100 µs
Leave(100 ms)
```

Empty (all-ones) blocks are skipped, because writing only turns bits 1 → 0. This also makes it possible to
append over already written data if only ones are changed to zeros (used during
calibration and when writing the fuse).

**WriteBlock — FLASH_1** (PFS154/PFS173: block_size = 4, groups = 1, clocks = 8):

```
for i in 0..block_size-1: send(wbuf[i], data_bits)   on PA6
send(aligned_addr, addr_bits)                         on PA6
PA6 → input;  4 µs
repeat clock_groups:
    repeat clocks_per_group:  CLK=1 15 µs; CLK=0 15 µs
    +1 clock;  4 µs
PA6 → output
25 µs
```

**WriteBlock — FLASH_2** (PFS172: block_size = 4, groups = 1, clocks = 4) — **the address goes before the data**:

```
send(aligned_addr, addr_bits)                         on PA6
for i: send(wbuf[i], data_bits)                       on PA6
PA6 → input
repeat clock_groups:
    +1 clock; 2 µs
    repeat clocks_per_group:  CLK=1 40 µs; CLK=0 40 µs
    repeat 4:                 CLK=1 2 µs;  CLK=0 2 µs
PA6 → output
25 µs
```

**WriteBlock — OTP1_2 / OTP2_x** (block_size = 2, groups = 1, clocks = 8):

```
for i: send(wbuf[i], data_bits)                       on PA4 (PA3 clock)
send(aligned_addr, addr_bits)                         on PA4
+1 clock;  4 µs
repeat clock_groups:
    CLK = 1   (held!)
    repeat clocks_per_group:  PA4=1 30 µs; PA4=0 30 µs     ← programming pulses on the data line
    CLK = 0;  4 µs
    +1 clock;  4 µs
25 µs
```

**OTP3_1** — writing is not implemented in the firmware (TODO).

**Time estimate:** PFS173 (3072 words) ≈ 768 blocks × (~0.3 ms of pulses + 0.1 ms + transfer) ≈ 0.4 s.
The host allows only 1000 ms for `W` — a new implementation must not be significantly slower.

### 4.10 Fuse and protection

* The fuse word is the **last word** of program memory (`codewords - 1`).
* Fuse bit 0 = 0 enables read protection (the host clears it only with `--allowsecfuse`).
* The fuse is written by a **separate** `W` command, last, after calibration.
* The `exclude_code_start…end` areas are reserved: in the FLASH ICs 0x7E0…0x7EF (PFS154/PFS172) and
  0xBE0…0xBEF (PFS173) — an OTP area with the factory BGTR/IHRCR values; in OTP — the final words.
  They are skipped during blank check and verify.

---

## 5. The full host algorithm (`easypdkprog`)

### 5.1 Opening

1. Open COM, set DTR, read and discard incoming data.
2. `I` → check PROTO 1.3.

### 5.2 `probe`

`P` → a table lookup by the rules of 4.6.

### 5.3 `read`

1. `R` (addr = 0, data_offs = 0, count = codewords, voltages `*_cmd_read` / `*_read_hv`).
2. The result must equal `id12bit`.
3. `G` in chunks → a file (Intel HEX 8-bit: byte addresses, a word = 2 bytes LE, or raw binary).

### 5.4 `write`

1. Read the HEX into a byte array; undefined bytes = `0xFF` (with `--securefill` = `0x00`, i.e. NOP,
   up to `min(codewords - 8, exclude_code_start)`).
2. If the fuse is not set and the file occupies all memory — the last word becomes the fuse and is removed from the data.
3. Find and replace the calibration stubs (up to 16, section 6).
4. Substitute the serial number (section 8).
5. FLASH (unless `--noerase`): `E` → result = ID.
6. `Z` (unless `--noblankchk`) over the whole range, taking exclude into account.
7. `S` (data from byte 0) → `W` (addr 0, data_offs 0, count = ⌈len/2⌉, block parameters from the table).
8. `V` (unless `--noverify`).
9. For each calibration, in order of increasing address:
   `C` → check that the frequency is within ±10 % of the target → patch the code (section 6.4) → `S` of the whole
   array → `W` of the whole array (only zeros are added).
10. The fuse (if present): `S` of 2 bytes at byte offset `(codewords-1)*2` → `W` with
    `addr = data_offs = codewords-1`, `count = 1`.

### 5.5 `erase`

`E`, then `Z`.

> ⚠️ In `easypdkprog` 1.3, in the `erase` branch the blank check is called with swapped arguments
> (`vdd_read = vpp_cmd_read`, `vpp_read = vdd_read_hv`). Do not copy this.

### 5.6 `start`

`X` with `--runvdd` (default 5.0 V) → a loop: print the `D` packets, send pressed keys as `D` packets;
Esc → `Q`.

---

## 6. Oscillator calibration (IHRC / ILRC / BandGap)

PADAUK ICs have no factory IHRC calibration in the program — the programmer performs it **after writing,
on the working IC**. For this a "marker" is embedded in the user firmware, which the host replaces with
a special measurement loop, writes, measures, and then turns the loop into a simple register setting.

### 6.1 The marker in the user program (from the SDCC macros `easypdk/pdkcommon.h`)

**IHRC/ILRC** — 10 `AND A, #imm` instructions:

```
and a, #'R'
and a, #'C'
and a, #type            ; 1 = IHRC, 2 = ILRC
and a, #freq[7:0]       ; target SYSCLK, Hz
and a, #freq[15:8]
and a, #freq[23:16]
and a, #freq[31:24]
and a, #mV[7:0]         ; calibration VDD, mV
and a, #mV[15:8]
and a, #reg             ; IO address of the trim register (IHRCR=0x0B, ILRCR=0x39 for PFS154 ...)
```

**BandGap** — 14 instructions:

```
and a, #'B' / 'G' / 3 / (3950 & 0xFF) / (3950 >> 8) / bgtr / gpcc / gpcs / 0 / 0 / 0 / 0 / 0 / 0
```

The `AND A, #imm` opcode depends on the core width: 13 bit — `0x14xx`, 14 bit — `0x2Cxx`,
15 bit — `0x54xx`, 16 bit — `0x1Cxx` (calibration is not supported for 16 bit — the host reports an error).

### 6.2 The measurement loop (written in place of the marker)

**IHRC/ILRC** (for a 14-bit core as an example; `pos` is the marker address):

| pos+ | Opcode | Instruction | After calibration |
|---|---|---|---|
| 0 | `0x1ED1` | `SET1 PAC.3` — PA3 output | NOP |
| 1 | `0x1F0D` | `SET1 PADIER.4` — PA4 digital input | NOP |
| 2 | `0x2FFF` | `MOV A, #0xFF` | `MOV A, #value` |
| 3 | `0x2801` | `ADD A, #1` | NOP |
| 4 | `0x1ED0` | **loop:** `SET1 PA.3` | NOP |
| 5 | `0x0180\|reg` | `MOV IO(reg), A` — write to IHRCR/ILRCR | **stays** |
| 6 | `0x1910` | `T0SN PA.4` — skip the next one if PA4 = 0 | NOP |
| 7 | `0x2801` | `ADD A, #1` | NOP |
| 8 | `0x1CD0` | `SET0 PA.3` | NOP |
| 9 | `0x3004+pos` | `GOTO loop` | NOP |

The loop body is exactly **7 clocks** (5 single-clock + GOTO at 2), so the IC generates a square wave on PA3
`f_PA3 = SYSCLK / 7`. Every time PA4 is one during `T0SN`, A is incremented and the next value
is written to the trim register.

The opcode tables for the 13/14/15-bit cores are in [fpdkiccalib.c](../easy-pdk-programmer-software/fpdkiccalib.c);
the host substitutes them itself, and the programmer firmware does not need to know the code contents.

**BandGap**: PA0 and PA3 are outputs (`MOV A,#0x09; MOV PAC,A`), PA4 is an input, the comparator is enabled
(`GPCC = 0x84`, `GPCS = 0xA3`), the comparator output goes to PA0; the loop is similar but writes to BGTR.

### 6.3 What the programmer does in the `C` command

1. Set up the counter (in the original SPI1 **slave**): SCK ← IC PA3, MISO → IC PA4,
   circular DMA over `block` bytes of TX and RX, MSB first, CPOL = 0, CPHA = 0. Each SPI bit = one turn of the IC loop.
2. VPP is not applied. VDD = `vdd_mV`, wait 1 ms — the IC starts, runs the written program, and reaches the loop.
3. **A "step"** (`Next(n)`, n ≤ 16 at a time): `(1 << n) - 1` is written into the first 2 bytes of the TX buffer
   (byte0 = least significant), that is, n one bits on PA4 → the register value grows by n. After the transfer
   the buffer is zeros again. Waiting for confirmation takes up to 1 s.
4. **Frequency measurement** (`Measure()`): one step of 1 is made, 1 block is skipped, then a
   48 MHz timer measures the time `t` of `M - 1` full blocks:
   `f_PA3 = 48 000 000 × 8 × block × (M - 1) / t`, and `SYSCLK = f_PA3 × mult` (mult = 7).
   The window size depends on the target frequency:

   | Target SYSCLK | block, bytes | M (blocks) |
   |---|---|---|
   | > 4 MHz | 16 | 24 |
   | ≤ 4 MHz | 16 | 8 |
   | ≤ 1 MHz | 16 | 4 |
   | ≤ 100 kHz | 4 | 2 |

5. The search:
   * **IHRC** — values 1…0x9F in steps of 1 (value 0 is skipped, because the first measurement is already after the
     first step), the one closest to the target frequency is chosen;
   * **ILRC** — 0x00…0xF0 in steps of 0x10 (only the upper 4 bits are used);
   * **BG** — 0x00…0xF0 in steps of 0x10: after each step wait 500 µs and read the IC's PA0;
     the first value at which PA0 = 0 is the result. Not found → `'E'`.
   * If the measurement returned 0 (no clocks for 1 s) — the search stops.
6. VDD = 0, stop the counter, wait 50 ms. Reply with `value`, `freq`.

When implementing on another MCU it is enough to: (a) count edges on the IC's PA3 over a known time (a timer in
external-clock mode / input capture), (b) be able to output exactly n ones on
PA4 synchronously with the PA3 edges (for example, from a PA3 edge interrupt — but the PA3 frequency is up to ~2.3 MHz at SYSCLK 16 MHz, so it is
better in hardware: SPI slave, a timer with DMA, and so on).

### 6.4 The patch after calibration (on the host)

* instructions marked `CO_AC_NOP` → `0x0000` (NOP);
* `MOV A, #0xFF` → `MOV A, #value`;
* `MOV IO(reg), A` stays.

All these changes only clear bits, so a repeated `W` without erasing is correct. As a result the program
is left with `MOV A,#value; MOV IHRCR,A` and NOPs.

---

## 7. Run mode (the UART bridge)

* `X`: VDD = `vdd_mV`, VPP = 0, UART 8N1 with **baud-rate auto-detection from the start bit** (STM32 ABR mode 0):
  * IC TX → **PA7** → the programmer input (with a pull-down);
  * the programmer output → **PA0** → IC RX.
* The first character from the IC must have its least significant bit = 1 (by convention `'U'` = 0x55 is sent); it is measured and
  discarded (if it is indeed `'U'`).
* After detection the programmer sends the host a `'D'` packet `"Connected @<baud> baud\n"`.
  On an auto-detection error the UART is restarted.
* After that all bytes from the IC are collected into a 32-byte ring queue and forwarded in `'D'` packets.
* `'D'` packets from the host are sent to the IC at the same baud rate.
* `Q` or closing the port — turn off the UART and VDD.

---

## 8. The serial number

The host looks in the code for 8 consecutive instructions `RET #'F'`, `RET #'P'`, `RET #'S'`, `RET #'E'`, `RET #'R'`,
`RET #'I'`, `RET #'A'`, `RET #'L'` (the `RET k` opcode: 13 bit — `0x01xx`, 14/15 bit — `0x02xx`,
16 bit — `0x0Fxx`) and replaces the low bytes with the bytes of a 64-bit serial number (`--serial`),
least significant byte first. It is performed entirely on the host; the programmer is not involved.

---

## 9. The IC table (from `fpdkicdata.c`)

Voltages in volts: `cmd` = (VDD / VPP) during entering and the command, `hv` = (VDD / VPP) during the operation.
The write block = `block_size / clock_groups / clocks_per_group`. RO — writing is not supported in v1.3.

| IC | ID12 | Type | addr / data bits | Words | Read cmd → hv | Write cmd → hv | Block | Erase cmd → hv, clocks | Exclude |
|---|---|---|---|---|---|---|---|---|---|
| PMS131 / PMC131 | 0xC83 | 0x22 | 12 / 14 | 0x600 | 3.0/5.0 → 3.0/5.0 | RO | — | — | 0x5F9–0x600 |
| PMS132 / PMS132B | 0x109 | 0x22 | 12 / 14 | 0x800 | 3.0/5.0 → 3.0/5.0 | RO | — | — | 0x7F6–0x800 |
| PMS133 / PMS134 | 0xC19 | 0x21 | 12 / 15 | 0x1000 | 3.0/5.0 → 3.0/5.0 | RO | — | — | 0xFF6–0x1000 |
| PMS150C / PMS15A | 0xA16 | 0x12 | 12 / 13 | 0x400 | 3.0/5.0 → 3.0/5.0 | 4.3/8.0 → 5.8/10.5 | 2/1/8 | — | 0x3F6–0x400 |
| PMS152 | 0xA27 | 0x22 | 12 / 14 | 0x500 | 3.0/5.0 → 3.0/5.0 | 4.3/8.0 → 5.8/10.5 | 2/1/8 | — | 0x4F6–0x500 |
| PMS154B / PMS154C | 0xE06 | 0x12 | 12 / 14 | 0x800 | 3.0/5.0 → 3.0/5.0 | 4.3/8.0 → 5.8/11.0 | 2/1/8 | — | addr. 0 + 0x7E8–0x800 |
| **PFS154** | 0xAA1 | 0xF1 | 13 / 14 | 0x800 | 2.5/5.5 → 2.5/5.5 | 2.5/5.5 → 5.8/8.5 | 4/1/8 | 2.5/5.5 → 3.0/9.0, 2 | 0x7E0–0x7F0 |
| PMS171B | 0xD36 | 0x22 | 12 / 14 | 0x600 | 3.0/5.0 → 3.0/5.0 | 4.3/8.0 → 5.8/10.5 | 2/1/8 | — | 0x5F6–0x600 |
| PFS172 | 0xCA6 | 0xF2 | 13 / 14 | 0x800 | 2.5/5.5 → 2.5/**0.0** | 2.5/5.5 → 5.3/**0.0** | 4/1/4 | 2.5/5.5 → 5.3/**0.0**, 14 | 0x7E0–0x7F0 |
| PFS173 | 0xEA2 | 0xF1 | 13 / 15 | 0xC00 | 2.5/5.5 → 2.5/5.5 | 2.5/6.5 → 5.8/9.0 | 4/1/8 | 2.5/5.5 → 3.0/9.0, 4 | 0xBE0–0xBF0 |
| PMC251 | 0x058 | 0x31 | 13 / 16 | 0x400 | 2.5/5.0 → 2.5/5.5 | RO | — | — | 0x3F9–0x400 |
| PMS271 / PMC271 | 0xA58 | 0x31 | 13 / 16 | 0x400 | 2.5/5.0 → 2.5/5.5 | RO | — | — | 0x3F9–0x400 |
| MCU390 | 0xC31 | 0x12 | 12 / 14 | 0x800 | 3.0/5.0 → 3.0/5.0 | RO | — | — | 0x700–0x800 |

Notes:

* The PFC154 (there is a datasheet in `docs/`) is **absent** from the version 1.3 table — it needs parameters from
  newer free-pdk versions or your own research.
* The `otpid` field (e.g. 0x2AA1 for PFS154) is the ID in the notation of the original PADAUK programmer; it is not
  used in the protocol.

---

## 10. Example exchange (PFS154, read)

```
PC → 'I' 00
   ← 'A' 2D 00 "FREE-PDK EASY PROG - HW:1.2 SW:1.3 PROTO:1.3\n"

PC → 'R' 1B
     A1 0A            icid = 0x0AA1
     F1               type = FLASH_1
     C4 09 00 00      vdd_cmd  = 2500 mV
     7C 15 00 00      vpp_cmd  = 5500 mV
     C4 09 00 00      vdd_read = 2500 mV
     7C 15 00 00      vpp_read = 5500 mV
     00 00            addr = 0
     0D               addr_bits = 13
     00 00            data_offs = 0
     0E               data_bits = 14
     00 08            count = 0x0800
   ← 'A' 02 00 A1 0A  (success, ID echo)

PC → 'G' 04 00 00 3F 00     (offset 0, 63 bytes)
   ← 'A' 3F 00 <63 bytes>
   ...
```

On the IC lines this looks like: PA3 = 0 → VPP 5.5 V → 100 µs → VDD 2.5 V → 500 µs → 32 bits `0xA5A5A5A6` on PA6 →
16 ACK bits (`xxxx 1010 1010 0001`) → for each address 13 address bits + 5 µs + 14 data bits + a clock →
remove VDD/VPP → 10 ms.

---

## 11. Known quirks and pitfalls of the original implementation

1. The bounds check of `S`/`G` in the firmware mistakenly multiplies the buffer size by 2 — the actual buffer is 8192 bytes;
   check correctly in your implementation.
2. `E` for OTP returns `0xFFFF` **without leaving programming mode** (VPP/VDD stay on).
   It is better to check the type first and then apply voltage.
3. The host uses a 1000 ms timeout for `W` (as for read), not 2000.
4. `Z`/`R`/`V`: the host sends byte 12 = `vdd_cmd >> 8` instead of `vdd_read >> 8` (see 2.4).
5. In verify, `excl_first` is compared with the absolute address, while the exclude range is compared with the relative index `p`.
6. An error/timeout during a single operation makes the host's write process hang; operations must be
   atomic on the programmer side and always end with `Leave()` (removing the voltages).
7. `FPDK_SetVDD/SetVPP` always return success — there is no hardware voltage monitoring; on overload
   (an incorrectly inserted IC) there is no protection. New hardware can add current/voltage monitoring
   and return `0xFFFE`/`0xFFFD`.
