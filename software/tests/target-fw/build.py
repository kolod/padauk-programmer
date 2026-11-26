#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the PADAUK test programs of software/tests/target-fw with SDCC (pdk14 / pdk15 ports).

The free-pdk headers (pdk/device.h, easy-pdk/calibrate.h) are not in this repository: they are taken from
$PDK_INCLUDES, else from the PlatformIO easypdk framework (~/.platformio/packages/framework-easypdk/includes).

Usage: python build.py [--out DIR]       # → DIR/<name>.ihx for every target below (default DIR: build/target-fw)
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]

# name -> (source, chip define, sdcc port, extra defines)
TARGETS = {
    "clockout_8mhz_pfs154": ("clockout/clockout.c", "PFS154", "pdk14", ["F_CPU=8000000"]),
    "clockout_1mhz_pfs154": ("clockout/clockout.c", "PFS154", "pdk14", ["F_CPU=1000000"]),
    "clockout_ilrc_pfs154": ("clockout/clockout.c", "PFS154", "pdk14", ["USE_ILRC", "F_CPU=55000"]),
    "bandgap_pfs154": ("bandgap/bandgap.c", "PFS154", "pdk14", []),
    "uart_pfs154": ("uart/uart.c", "PFS154", "pdk14", []),
}


def includes() -> pathlib.Path:
    env = os.environ.get("PDK_INCLUDES")
    p = pathlib.Path(env) if env else pathlib.Path.home() / ".platformio/packages/framework-easypdk/includes"
    if not (p / "pdk" / "device.h").exists():
        sys.exit(f"free-pdk headers not found in {p}: set PDK_INCLUDES to the folder holding pdk/ and easy-pdk/")
    return p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "target-fw"))
    a = ap.parse_args()
    sdcc = shutil.which("sdcc")
    if not sdcc:
        sys.exit("sdcc not found in PATH (SDCC ≥ 4.1 with the pdk14 port)")
    inc = includes()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for name, (src, chip, port, defs) in TARGETS.items():
        cmd = [sdcc, f"-m{port}", f"-D{chip}", f"-I{inc}", "--opt-code-size", *[f"-D{d}" for d in defs],
               "-o", str(out / f"{name}.ihx"), str(HERE / src)]
        r = subprocess.run(cmd, cwd=out)
        if r.returncode:
            return r.returncode
        print(out / f"{name}.ihx")
    return 0


if __name__ == "__main__":
    sys.exit(main())
