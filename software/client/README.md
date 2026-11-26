# pdkprog — PADAUK programmer client

A Python ≥ 3.8 client (Windows 7 and later, Linux, macOS). All IC information comes from the programmer
firmware (`LIST_CHIPS`), so a new IC does not need a new client. The protocol is
[`docs/protocol.md`](../../docs/protocol.md).

## Installation

```sh
pip install -e software/client          # from the repository; the dependency is pyserial
pip install -e "software/client[dev]"   # + pytest for the tests
```

The port is found automatically (USB `1209:0001`); otherwise use `-p COM5`, `-p /dev/ttyACM0`, or `-p tcp://127.0.0.1:5555`
for the `pdk-sim` simulator ([`software/firmware/README.md`](../firmware/README.md)). On Windows 7 the COM port
needs [`software/drivers/windows/padauk-programmer.inf`](../drivers/windows/).

## Subcommands

| Command | Example | What it does |
|---|---|---|
| `info` | `pdkprog info` | versions, limits, build capabilities, package and self-test state |
| `list` | `pdkprog list` | ICs from the firmware table |
| `write` | `pdkprog -n PFS154 write hello.ihx` | store the package and program the IC (`upload` + `program`) |
| `upload` | `pdkprog upload -n PFS154 hello.ihx --fuse 0x3FFD --runvdd 3.3` | only store the package in the programmer (then run locally without a PC) |
| `program` | `pdkprog program` | program the IC with the stored package |
| `verify` | `pdkprog verify` | compare the IC with the package (taking calibration and serial number into account) |
| `probe` | `pdkprog probe` | identify the IC in the socket |
| `erase`, `blankcheck` | `pdkprog erase -n PFS154` | erase FLASH / check that it is blank |
| `read` | `pdkprog read -n PFS154 dump.hex` (`-b` — raw bin) | read the IC into a file, with a CRC check |
| `copy` | `pdkprog copy --ihrc snap --bg restore` | copy the IC into a new package, restoring the calibration |
| `restore-master` | `pdkprog restore-master` | restore the reference IC if `copy --ihrc measure` was interrupted |
| `start` | `pdkprog start -r 3.3` | start the IC with a UART bridge: its text to the screen, the keyboard to it, Esc to stop |
| `patch` | `pdkprog patch -n PFS154 --expect-package 0x1F0=0x0123` | append individual words (only 1 → 0) after checking the contents |
| `pkg` | `pdkprog pkg info` / `read f.bin` / `delete` / `check` | the stored package |
| `status`, `monitor` | `pdkprog monitor --count 3` | state; live operations, including local ones (button, pipeline) |
| `stats` | `pdkprog stats` / `stats reset` | counters of successful and failed writes |
| `config` | `pdkprog config copy --ihrc snap --bg restore` | modes of the local `COPY` |
| `selftest` | `pdkprog selftest` | check all VDD/VPP levels with an empty socket |
| `calibrate`, `supply` | `pdkprog supply 5 8.5 --hold 30` | calibrate the supplies against an external multimeter; arbitrary VDD/VPP for checks (nothing connected!) |

`write`, `program`, and other operations show stages and progress, and at the end the calibration result, the serial
number, and the checksums of the IC contents. Exit code: 0 — success, 1 — operation error, 3 — no connection.

### `copy`

Modes for each oscillator: `--ihrc keep|target|snap|measure`, `--ilrc keep|target|measure`,
`--bg keep|restore` (default `restore`); `target` requires `--ihrc-freq` / `--ilrc-freq`.

- `keep` — copies get the reference's trim value as is (the oscillator spread remains);
- `target` — copies are calibrated to the given frequency;
- `snap` — to the nearest standard one (16, 8, 4, 2, 1 MHz, 500 kHz), if the copy at the reference value is within 10 %;
- `measure` — to the reference's actual frequency: the programmer rewrites the reference FLASH once per stub and once
  more to restore it (`--allow-modify-master`, erase endurance). If interrupted, run `pdkprog restore-master`.

If calibration in the reference is not finished (measurement loop), `copy` offers to cancel or to set a target.

## Mapping to `easypdkprog`

| `easypdkprog` | `pdkprog` |
|---|---|
| `list`, `probe` | the same |
| `-n X read f` | the same (Intel HEX; `-b` — bin) |
| `-n X write f [--fuse --allowsecfuse --securefill --noerase --noblankchk --noverify]` | the same; the package is stored in the programmer and later written without a PC |
| `-n X erase` | the same |
| `start [-r V]` | the same (`-r` in volts; Esc stops) |
| `--nocalibrate` | not supported: remove the calibration macro from the program |
| `--serial` | not needed: each `EASY_PDK_SERIALNUM` stub gets a random 64-bit number |

The `easypdkprog` argument order (`-n X write f`) and the usual one (`write -n X f`) are equivalent.
The client rejects `--noblankchk` for OTP: rewriting an OTP is possible only after a content compatibility check.

## Calibration and serial number

- The `EASY_PDK_CALIBRATE_IHRC/ILRC/BG` macros from `easypdk/calibrate.h` stay in the program: the programmer
  finds the stub, writes a measurement loop in its place, calibrates, and appends the result. There is no need to
  remove them; a program without stubs is written with a warning.
- `EASY_PDK_SERIALNUM` gives each IC its own random number; rewriting an OTP keeps the number already
  written. `copy` of a reference that has a number copies it as is (warning `SERIAL_DUPLICATED`).
- If calibration fails, the stub remains a measurement loop and the IC can be finished later
  (`program` gives `RESUMED`); `--finalize-on-calib-fail` writes the best value found.

## Tests

```sh
pytest software/client/tests/unit
pytest software/client/tests --sim build/host/pdk-sim     # + contract tests against the simulator
PDK_FAST=1 pytest software/client/tests --sim build/host/pdk-sim   # fewer fault-injection iterations
```
