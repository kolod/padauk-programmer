# SPDX-License-Identifier: GPL-3.0-or-later
"""pdkprog: command-line client. All chip knowledge comes from the programmer (LIST_CHIPS)."""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

from . import __version__, ihex, package
from .protocol import (CAP_SUPPLY_CAL, FAMILY_NAMES, PkgState, Programmer, ProgrammerError, ProtocolError, Status,
                       connect, status_name)
from .transport import TransportError

PKG_MESSAGES = {
    Status.PKG_CHIP_UNSUPPORTED: "the programmer does not know this chip",
    Status.PKG_TOO_LARGE: "the image does not fit the chip or the package slot",
    Status.PKG_HEADER_INVALID: "invalid package header",
    Status.PKG_OFFSET: "upload out of sync",
    Status.PKG_CRC_MISMATCH: "package CRC mismatch after storing — the upload was damaged",
    Status.PKG_RESERVED_AREA: "the image writes into the chip's reserved area",
    Status.PKG_FUSE_CONFLICT: "the last image word (fuse) contradicts --fuse",
    Status.PKG_SECURITY_NOT_ALLOWED: "the fuse enables read protection; add --allowsecfuse to allow it",
    Status.PKG_CALIB_UNKNOWN: "unrecognised calibration placeholder (6+ AND A,#k in a row)",
    Status.PKG_CHIP_READ_ONLY: "this chip can only be read",
    Status.PKG_OTP_BLANK_CHECK_REQUIRED: "OTP chips are always blank-checked",
    Status.STORE_WRITE_FAILED: "programmer flash write failed",
    Status.STORE_EMPTY: "no package stored",
    Status.STORE_CORRUPTED: "stored package is corrupted",
    Status.MASTER_RESTORE_PENDING: "a master chip restore is pending; run restore-master first",
    Status.BUSY: "the programmer is busy",
    Status.CALIB_WRONG_SITE: "the chip runs a different calibration placeholder than the one being calibrated - "
                             "placeholders must be executed in address order (see docs/padauk-calibration.md)",
}


class CliError(Exception):
    pass


def describe_error(e: ProgrammerError) -> str:
    msg = PKG_MESSAGES.get(e.status, "")
    text = f"{status_name(e.status)}" + (f": {msg}" if msg else "")
    if e.status in (Status.PKG_RESERVED_AREA, Status.PKG_FUSE_CONFLICT, Status.PKG_SECURITY_NOT_ALLOWED,
                    Status.PKG_CALIB_UNKNOWN, Status.PKG_CRC_MISMATCH) and len(e.payload) >= 2:
        (addr,) = struct.unpack_from("<H", e.payload)
        if addr != 0xFFFF:
            text += f" (word address 0x{addr:04X})"
    if e.status == Status.PKG_OFFSET and len(e.payload) >= 4:
        text += f" (programmer expected offset {struct.unpack_from('<I', e.payload)[0]})"
    return text


def read_image(path: str) -> dict[int, int]:
    ext = os.path.splitext(path)[1].lower()
    if ext in (".bin", ".raw"):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) % 2:
            raise CliError("binary image must have an even length")
        return {i: struct.unpack_from("<H", data, 2 * i)[0] for i in range(len(data) // 2)}
    try:
        return ihex.read_words(path)
    except ihex.IhexError as e:
        raise CliError(f"{path}: {e}") from None


def parse_int(s: str) -> int:
    return int(s, 0)


# ------------------------------------------------------------------ commands
def cmd_upload(prog: Programmer, a) -> int:
    if a.nocalibrate:
        raise CliError("--nocalibrate is not supported: every calibration placeholder is calibrated. "
                       "To skip calibration, remove the EASY_PDK_CALIBRATE_* macro from the program.")
    if a.serial is not None:
        raise CliError("--serial is not supported: the programmer writes a random 64-bit serial number "
                       "into every EASY_PDK_SERIALNUM placeholder automatically.")
    chip = prog.find_chip(a.icname) if a.icname else None
    if chip is None:
        raise CliError("-n/--icname is required")
    if not chip.can_write:
        raise CliError(f"{chip.name} can only be read")
    if a.noblankchk and not chip.is_flash:
        raise CliError(f"--noblankchk is not allowed for OTP chips ({chip.name} is "
                       f"{FAMILY_NAMES.get(chip.family, 'OTP')}): a blank check protects the chip")

    words = read_image(a.file)
    flags = 0
    if a.allowsecfuse:
        flags |= package.ALLOW_SECURITY_FUSE
    if a.noerase:
        flags |= package.SKIP_ERASE
    if a.noblankchk:
        flags |= package.SKIP_BLANK_CHECK
    if a.noverify:
        flags |= package.SKIP_VERIFY
    if a.securefill:
        flags |= package.SECURE_FILL
    with open(a.file, "rb") as f:
        src_crc = zlib.crc32(f.read())
    try:
        pkg = package.build(chip.id12, a.icname.upper(), words, a.name or package.default_name(a.file),
                            a.label or "", flags=flags, fuse=a.fuse, run_vdd_mV=int(a.runvdd * 1000),
                            source_crc32=src_crc, code_words=chip.code_words)
    except package.PackageError as e:
        raise CliError(str(e)) from None

    def progress(done, total):
        if a.verbose:
            print(f"\rUploading {done}/{total} bytes", end="", file=sys.stderr, flush=True)

    res = prog.upload_package(pkg, progress)
    if a.verbose:
        print(file=sys.stderr)
    if res["crc32"] != package.trailer_crc(pkg):
        raise CliError(f"CRC mismatch: programmer 0x{res['crc32']:08X}, package 0x{package.trailer_crc(pkg):08X}")
    h = package.Header.parse(pkg)
    print(f"Package '{h.name}' for {chip.name} stored: {h.image_words} words, CRC 0x{res['crc32']:08X}, "
          f"{res['effective_words']} words to write, calibration sites {res['calib_sites']}, "
          f"serial sites {res['serial_sites']}")
    if res["calib_sites"] == 0:
        print("warning: no calibration placeholder found — the chip will run with factory oscillator settings. "
              "If the program sets IHRC/ILRC itself, use EASY_PDK_CALIBRATE_* instead.", file=sys.stderr)
    return 0


def print_header(h: package.Header, state, crc: int) -> None:
    print(f"State:    {state.name}")
    if state == PkgState.EMPTY:
        return
    print(f"Name:     {h.name}")
    if h.label:
        print(f"Label:    {h.label}")
    print(f"Chip:     {h.chip_name} (0x{h.chip_id12:03X})")
    print(f"Words:    {h.image_words}")
    print(f"Size:     {h.package_size} bytes, CRC 0x{crc:08X}")
    print(f"Flags:    {' '.join(h.flag_names()) or '-'}")
    if h.flags & package.FUSE_PRESENT:
        print(f"Fuse:     0x{h.fuse:04X}")
    if h.run_vdd_mV:
        print(f"Run VDD:  {h.run_vdd_mV / 1000:.2f} V")
    print(f"Source:   CRC 0x{h.source_crc32:08X}")


def cmd_pkg(prog: Programmer, a) -> int:
    if a.action == "info":
        i = prog.package_info()
        h = package.Header.parse(i["header"]) if i["state"] != PkgState.EMPTY else None
        print_header(h, i["state"], i["crc32"])
        return 0 if i["state"] != PkgState.CORRUPTED else 2
    if a.action == "check":
        c = prog.package_check()
        print(f"{c['state'].name}, computed CRC 0x{c['crc32']:08X}")
        return 0 if c["state"] == PkgState.VALID else 2
    if a.action == "delete":
        prog.package_delete()
        print("Package deleted")
        return 0
    if a.action == "read":
        if not a.file:
            raise CliError("pkg read FILE")
        data = prog.package_read()
        try:
            package.check(data)
        except package.PackageError as e:
            raise CliError(f"read package is invalid: {e}") from None
        with open(a.file, "wb") as f:
            f.write(data)
        print(f"Package ({len(data)} bytes) saved to {a.file}")
        return 0
    raise CliError(a.action)


def cmd_info(prog: Programmer, a) -> int:
    i = prog.info
    print(f"Programmer: {i.platform}, firmware {i.firmware_version}, protocol {i.proto_major}.{i.proto_minor}")
    print(f"Max payload {i.max_payload} B, package {i.max_package_size} B, read {i.max_read_words} words, "
          f"capabilities 0x{i.capabilities:08X}, chips {i.chip_table_size}")
    p = prog.package_info()
    print(f"Package: {p['state'].name}" + (f", CRC 0x{p['crc32']:08X}" if p["state"] != PkgState.EMPTY else ""))
    return 0


def cmd_list(prog: Programmer, a) -> int:
    print("Supported ICs:")
    for c in prog.list_chips():
        kind = "FLASH" if c.is_flash else "OTP"
        ro = "" if c.can_write else " (RO)"
        print(f" {'/'.join(c.names):<16} (0x{c.id12:03X}): {kind}: {c.code_words}x{c.code_bits} bit, "
              f"RAM: {c.ram_bytes} bytes{ro}")
    return 0


CAL_NAMES = ("VDD measurement", "VPP measurement", "VDD output", "VPP output")


def ask(prompt: str) -> str:
    try:
        return input(prompt)
    except EOFError:
        return ""


def parse_volts(text: str) -> int:
    """'5.32', '5,32' or '5.32 V' → millivolts."""
    v = float(text.strip().lower().rstrip("v").strip().replace(",", "."))
    if not 0.0 < v < 30.0:
        raise ValueError(text)
    return round(v * 1000)


def show_calibration(c: dict) -> None:
    if not c["valid"]:
        print("Supplies are not calibrated.")
        return
    print("Supply calibration:")
    for name, pm in zip(CAL_NAMES, c["cal_permille"]):
        print(f"  {name:16} {pm / 10:+.1f} %")


def cmd_calibrate(prog: Programmer, a) -> int:
    if not prog.info.capabilities & CAP_SUPPLY_CAL:
        raise CliError("this programmer has no supply front end to calibrate")
    if a.show:
        show_calibration(prog.cal_get())
        return 0
    if a.reset:
        prog.cal_clear()
        print("Supply calibration cleared.")
        return 0

    print("WARNING: disconnect the programmer from any target board before calibrating.")
    print("Both supplies will be raised to near their maximum (VDD about 6 V, VPP about 12-13 V).")
    if not a.yes and ask("Nothing is connected to the output pins? [y/N] ").strip().lower() not in ("y", "yes"):
        print("Calibration cancelled.")
        return 1

    started = prog.cal_start()
    accepted = False
    try:
        print(f"Applied VDD = {started['vdd_set_mV'] / 1000:.2f} V and VPP = {started['vpp_set_mV'] / 1000:.2f} V "
              "(nominal). Measure both against GND with a multimeter.")
        while True:
            try:
                vdd = parse_volts(a.vdd) if a.vdd else parse_volts(ask("Measured VDD, V: "))
                vpp = parse_volts(a.vpp) if a.vpp else parse_volts(ask("Measured VPP, V: "))
            except ValueError:
                if a.vdd or a.vpp:
                    raise CliError("measured voltages must be numbers in volts") from None
                print("Please enter a number in volts, e.g. 5.93")
                continue
            try:
                res = prog.cal_set(vdd, vpp)
                break
            except ProgrammerError as e:
                if e.status != Status.BAD_PAYLOAD or a.vdd or a.vpp:
                    raise
                rail = "VDD" if e.payload[:1] == b"\x00" else "VPP"
                print(f"The {rail} value is more than 12.7 % away from what the programmer expects - "
                      "check the meter and type it again.")
        print(f"Now applied with the new calibration: VDD = {res['vdd_target_mV'] / 1000:.3f} V, "
              f"VPP = {res['vpp_target_mV'] / 1000:.3f} V.")
        print(f"(the programmer reads VDD {res['vdd_meas_mV'] / 1000:.3f} V, VPP {res['vpp_meas_mV'] / 1000:.3f} V)")
        if a.accept is not None:
            accepted = a.accept
        else:
            answer = ask("Does the meter show the same voltages (within about 0.5 %)? [y/N] ")
            accepted = answer.strip().lower() in ("y", "yes")
        prog.cal_confirm(accepted)
    except BaseException:
        if not accepted:
            try:
                prog.cal_confirm(False)   # voltages off, previous calibration kept
            except Exception:
                pass
        raise
    if accepted:
        print("Calibration stored.")
        show_calibration(prog.cal_get())
        return 0
    print("Calibration discarded; the previous one stays in effect.")
    return 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="pdkprog", description="PADAUK programmer client")
    p.add_argument("--version", action="version", version=f"pdkprog {__version__}")
    p.add_argument("-p", "--port", help="serial port or tcp://host:port (default: auto-detect USB 1209:0001)")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)

    u = sub.add_parser("upload", help="store a firmware package in the programmer")
    u.add_argument("file")
    u.add_argument("-n", "--icname", required=True, help="chip name as shown by 'list'")
    u.add_argument("--fuse", type=parse_int)
    u.add_argument("--allowsecfuse", action="store_true")
    u.add_argument("--securefill", action="store_true")
    u.add_argument("--noerase", action="store_true")
    u.add_argument("--noblankchk", action="store_true")
    u.add_argument("--noverify", action="store_true")
    u.add_argument("--runvdd", type=float, default=0.0, help="VDD for 'start' in volts")
    u.add_argument("--name", help="package name (default: file name)")
    u.add_argument("--label")
    u.add_argument("--nocalibrate", action="store_true", help=argparse.SUPPRESS)
    u.add_argument("--serial", help=argparse.SUPPRESS)
    u.set_defaults(func=cmd_upload)

    k = sub.add_parser("pkg", help="stored package: info | read FILE | delete | check")
    k.add_argument("action", choices=["info", "read", "delete", "check"])
    k.add_argument("file", nargs="?")
    k.set_defaults(func=cmd_pkg)

    c = sub.add_parser("calibrate", help="calibrate VDD/VPP with an external meter (nothing connected!)")
    c.add_argument("--show", action="store_true", help="print the stored calibration")
    c.add_argument("--reset", action="store_true", help="clear the stored calibration")
    c.add_argument("--yes", action="store_true", help="do not ask whether the outputs are disconnected")
    c.add_argument("--vdd", help="measured VDD in volts (otherwise asked)")
    c.add_argument("--vpp", help="measured VPP in volts (otherwise asked)")
    g = c.add_mutually_exclusive_group()
    g.add_argument("--accept", dest="accept", action="store_true", default=None,
                   help="store without asking (the check voltages were verified)")
    g.add_argument("--reject", dest="accept", action="store_false", help="discard without asking")
    c.set_defaults(func=cmd_calibrate)

    sub.add_parser("info", help="programmer information").set_defaults(func=cmd_info)
    sub.add_parser("list", help="chips supported by the programmer").set_defaults(func=cmd_list)
    return p


def main(argv: list[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    try:
        prog = connect(a.port, client=f"pdkprog {__version__}")
    except (TransportError, ProtocolError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    except ProgrammerError as e:
        if e.status == Status.PROTO_VERSION:
            print("error: the programmer speaks an incompatible protocol version; update pdkprog or the firmware",
                  file=sys.stderr)
        else:
            print(f"error: {describe_error(e)}", file=sys.stderr)
        return 3
    try:
        return a.func(prog, a)
    except ProgrammerError as e:
        print(f"error: {describe_error(e)}", file=sys.stderr)
        return 1
    except (CliError, KeyError) as e:
        msg = f"unknown chip {e}" if isinstance(e, KeyError) else str(e)
        print(f"error: {msg}", file=sys.stderr)
        return 1
    except (ProtocolError, TransportError) as e:
        print(f"error: connection lost: {e}", file=sys.stderr)
        return 3
    finally:
        prog.close()


if __name__ == "__main__":
    sys.exit(main())
