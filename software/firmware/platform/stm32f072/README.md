# STM32F072 platform (Easy PDK boards)

Debug target: firmware for Easy PDK programmer boards based on the STM32F072C8T6. The wiring is taken from
`easy-pdk-programmer-software/Firmware` (as a reference only). The IC engine here is a stub
(`platform/common/chip_engine_stub.c`): all IC operations answer `OP_UNSUPPORTED`.

## Pins

| Pin | Function | Purpose | Note |
|---|---|---|---|
| PA4 | DAC_OUT1 | reference voltage of the VPP regulator | 0…4095 → 0…13.27 V |
| PA5 | DAC_OUT2 | reference voltage of the VDD regulator | 0…4095 → 0…6.26 V |
| PA2 | GPIO, output | enable of the 15 V boost converter (for VPP) | 1 = enabled |
| PA3 | TIM15_CH2 (AF0) | PWM ≈ 45 kHz, 50 % → −4.8 V charge pump for the op-amp | |
| PB0 | ADC_IN8 | VPP measurement via a /6 divider | Easy PDK / Lite board |
| PB1 | ADC_IN9 | VDD measurement via a /6 divider | Easy PDK / Lite board |
| PA0 | ADC_IN0 | VPP measurement via a /6 divider | mini pill variant only |
| PA1 | ADC_IN1 | VDD measurement via a /6 divider | mini pill variant only |
| PB8 | GPIO, input with pull-up | board variant detection | to ground → Lite |
| PB9 | GPIO, input with pull-up | board variant detection | to ground → mini pill |
| PB3 | — | IC line PA3 / CLK | Hi-Z only for now |
| PB4 | — | IC line PA4 | Hi-Z only for now |
| PB5 | — | IC line PA6 / DAT | Hi-Z only for now |
| PB6 | USART1_TX (AF0) in `RUN` | IC line PA0: UART programmer → IC | Hi-Z outside `RUN` |
| PB7 | USART1_RX (AF0) in `RUN` | IC line PA7: UART IC → programmer | Hi-Z outside `RUN`; baud rate auto-detected from the start bit of the first `'U'` |
| PC13 | GPIO, output | LED1 | active low |
| PC14 | GPIO, output | LED2 | active low |
| PC15 | GPIO, output | LED3 | active low |
| PA15 | GPIO, input with pull-down | button → local programming (`PROGRAM`) | pressed = 1 |
| PA11 / PA12 | USB DM / DP | USB CDC, VID/PID `1209:0001` | |
| PA13 / PA14 | SWDIO / SWCLK | debug | |
| PF0 / PF1 | OSC_IN / OSC_OUT | 8 MHz crystal | HSI without a crystal |

Internal resources: VREFINT (ADC_IN17) for computing VDDA, the temperature sensor (ADC_IN16) and VREFINT are
the entropy source, CRC is the hardware unit, TIM2 is the microsecond timer, SysTick gives milliseconds.

After reset, the first step (`Reset_Handler`, even before RAM initialization) sets the DAC to 0 V,
and the IC lines PB3…PB7 become inputs.

## Indication

| State | LED1 | LED2 | LED3 |
|---|---|---|---|
| ready (`READY`) | ● | | |
| operation (`BUSY`) | | ● | |
| success (`SUCCESS`) | ● | ● | |
| error (`FAIL`) | | | ● |
| local start blocked (`BLOCKED`) | ● | | ● |

Temporary display; the final indication is task T102.

## Clocking

SYSCLK 48 MHz: HSE 8 MHz × 6 (or HSI / 2 × 12 if the crystal did not start). USB runs from HSI48 with
CRS trimming from SOF frames. The ADC runs from HSI14.

## Memory

| Address | Size | Contents |
|---|---|---|
| `0x08000000` | 44 KB | firmware |
| `0x0800B000` | 8 KB | package slot 0 |
| `0x0800D000` | 8 KB | package slot 1 |
| `0x0800F000` | 2 KB | state log, page A |
| `0x0800F800` | 2 KB | state log, page B |

A flash page is 2 KB, written in half-words (the write unit is 2 bytes). Erase is synchronous: it
runs in the background between commands, but the CPU stalls for the duration of a page erase (≈ 20–40 ms).

## Build

```
cmake -B build/f072 -G Ninja -DPLATFORM=stm32f072 \
      -DCMAKE_TOOLCHAIN_FILE=<absolute path>/software/firmware/cmake/arm-none-eabi.cmake software/firmware
cmake --build build/f072
```

The output is `build/f072/pdkprog.elf` and `pdkprog.bin`.
