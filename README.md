# PADAUK Smart Programmer

A standalone programmer for PADAUK microcontrollers (PFS/PMS/PMC) with a USB CDC interface and a PC client.
Inspired by the [free-pdk / Easy PDK Programmer](https://free-pdk.github.io) project, but not a copy of it.

## Layout

| Directory | Contents |
|---|---|
| `hardware/` | schematic, board, enclosure |
| `software/` | programmer firmware and PC client |
| `docs/` | documentation, datasheets, protocol descriptions |
| `easy-pdk-programmer-*` | the original free-pdk projects (git submodules, reference only) |
| `specs/` | feature specifications (requirements, plan, tasks) |

## Documentation

- [PC ⇄ programmer protocol and package format](docs/protocol.md) — for a custom client or a firmware port
- [`pdkprog` client](software/client/README.md) — installation and subcommands, mapping to `easypdkprog`
- [Firmware](software/firmware/README.md) — build, the `pdk-sim` simulator, DFU, flash layout

## Licenses

| Part | License | File |
|---|---|---|
| Hardware (`hardware/`) | [CERN-OHL-S-2.0](https://ohwr.org/cern_ohl_s_v2.txt) | [LICENSE-HARDWARE](LICENSE-HARDWARE) |
| Software (`software/`) | [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html) | [LICENSE-SOFTWARE](LICENSE-SOFTWARE) |

Documentation in `docs/` created in this project is distributed under the license of the part it
describes. The PADAUK datasheets in `docs/` belong to their rights holders and are not covered by
the project licenses. The `easy-pdk-programmer-*` submodules have their own licenses
(CC-BY-SA-4.0 / GPL-3.0).

Hardware design source (Source Location per CERN-OHL-S-2.0):
<https://github.com/kolod/padauk-programmer>
