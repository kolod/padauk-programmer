# Programmer firmware

The core is C11 with no hardware dependencies (`core/`), hardware interfaces (`hal/`), and platforms (`platform/`):

| Platform | What it is | IC engine |
|---|---|---|
| `host` | the `pdk-sim` simulator (protocol over TCP) and unit tests | IC model `platform/host/chip_model.c` |
| `stm32g474` | WeAct STM32G474 board + `hardware/stm32g474-board` — the programmer | the PADAUK engine `core/engine` (feature `002-chip-engine`) over `platform/stm32g474/hal_g474_icp.c` |

The protocol and package format are in [`docs/protocol.md`](../../docs/protocol.md).

## Build

Requires CMake ≥ 3.20 and gcc/clang (MinGW-w64 on Windows; MSVC is not supported). For STM32,
`arm-none-eabi-gcc` ≥ 12. CMSIS and TinyUSB are downloaded at configure time (FetchContent, pinned
versions) and are not in the repository.

```sh
# simulator and unit tests
cmake -S software/firmware -B build/host -G Ninja -DPLATFORM=host
cmake --build build/host
ctest --test-dir build/host --output-on-failure

# STM32G474 (WeAct)
cmake -S software/firmware -B build/g474 -G Ninja -DPLATFORM=stm32g474 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/software/firmware/cmake/arm-cortex-m4f.cmake
cmake --build build/g474          # → build/g474/pdkprog-g474.bin
```

Build options:

| Option | Default | What it does |
|---|---|---|
| `PDK_FEATURE_MEASURE_MASTER` | `ON` | `COPY` in `MEASURE_MASTER` mode (bit 9 of `INFO.capabilities`; when off, mode 3 returns `OP_UNSUPPORTED`) |
| `PDK_FEATURE_PATCH` | `ON` | the `PATCH` operation (bit 6) |
| `PDK_VDD_TOL_MV` | `0` | board workaround: the VDD settle check accepts at least this deviation (the first G474 board, whose VDD reading wanders, is built with `300`); `0` = the protocol's ±2 % / ±30 mV rule. It masks a measurement fault — never a default |
| `PDK_G474_SUPPLIES` | `ON` | the VDD/VPP supplies of the `hardware/stm32g474-board` board; `OFF` — a bare WeAct module without supplies |

After linking, the build checks the size (`cmake/check_size.cmake`): `.text + .data` above 256 KB (bank 1) is an error
with a hint to disable features. The firmware currently takes about 37 KB.

## Flashing via DFU

The STM32G474 has a built-in USB DFU bootloader (`0483:df11`): hold BOOT0, press and release NRST, release BOOT0.

```sh
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/g474/pdkprog-g474.bin
```

After start, the programmer is a USB CDC device `1209:0001`. On Windows 7 the bootloader needs WinUSB (Zadig), and the
COM port needs [`software/drivers/windows/padauk-programmer.inf`](../drivers/windows/); Windows 10 and later
need no drivers. G474 details are in [`platform/stm32g474/README.md`](platform/stm32g474/README.md).

## Flash layout

A package is stored in a slot; the active slot and the counters are kept in the state log (40-byte records with their own CRC in
two pages, research R6). A new package is always written to the inactive slot and becomes active with a single log
record, so a power loss leaves either the old or the new package.

| Area | STM32G474 (512 KB, DBANK = 1) |
|---|---|
| firmware | bank 1: `0x08000000` – `0x0803FFFF` |
| package slots | 4 × 16 KB: `0x08040000`, `0x08044000`, `0x08048000`, `0x0804C000` |
| state log | `0x08050000`, `0x08051000` |

The storage is in bank 2: erasing does not stall the CPU (read-while-write). The simulator `pdk-sim` emulates a smaller
flash (2 slots of 8 KB), so `INFO.max_package_size` there is 8192. The slot size is given by
`INFO.max_package_size`.

## The `pdk-sim` simulator

```sh
build/host/pdk-sim --tcp 127.0.0.1:5555 --chip PFS154 --chip-state chip.bin
pdkprog --port tcp://127.0.0.1:5555 info
```

| Option | What it does |
|---|---|
| `--flash FILE` | emulated programmer flash in a file (survives restarts) |
| `--chip none \| NAME[:options]` | the IC in the socket; model options: `offset=±N` (oscillator offset, %), `osc=broken`, `weak=ADDR`, `order=reverse`, `deaf`, `pa0=float`, `slow=US`, `protected`, `halt=ADDR` (the program does not get past ADDR) |
| `--chip-state FILE` | IC memory in a file |
| `--power-fail-after N` | "cut the power" at the N-th flash or IC write operation (exit code 99) |
| `--power-fault RAIL:MODE`, `--power-scale RAIL:OUT:MEAS` | a faulty or inaccurate VDD/VPP supply |
| `--corrupt-flash-during-write` | flash read glitches while the IC is being written |

Commands from stdin (the programmer's controls): `local-start program|copy|verify|selftest|restore`,
`local-cancel`, `swap-chip SPEC` (insert another, blank IC). In `RUN` the IC model sends `'U'` and
`"hello\n"`, then echoes everything it receives; every erase and `UART_TX` byte is visible on stderr.
