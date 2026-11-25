# SPDX-License-Identifier: GPL-3.0-or-later
"""pdkprog: command-line client. All chip knowledge comes from the programmer (LIST_CHIPS)."""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time
import zlib

from . import __version__, ihex, package
from .protocol import (CALIB_KINDS, CAP_SUPPLY_CAL, CAP_SUPPLY_SET, FAMILY_NAMES, OPF_CALIB_FINALIZE_ON_FAIL,
                       OPF_SKIP_BLANK_CHECK, OPF_SKIP_ERASE, OPF_SKIP_VERIFY, SITE_STATUS, Op, PkgState, Programmer,
                       ProgrammerError, ProtocolError, Result, Status, connect, stage_name, status_name)
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


def parse_volts(text: str, allow_zero: bool = False) -> int:
    """'5.32', '5,32' or '5.32 V' → millivolts."""
    v = float(text.strip().lower().rstrip("v").strip().replace(",", "."))
    if not (0.0 <= v if allow_zero else 0.0 < v) or v >= 30.0:
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


RAILS = ("VDD", "VPP")


# ------------------------------------------------------------------ operations with the chip
OP_MESSAGES = {
    Status.CHIP_NOT_FOUND: "no chip answers - check the socket and the ICP wiring",
    Status.CHIP_ID_MISMATCH: "another chip type is in the socket",
    Status.CHIP_NOT_BLANK: "the chip is not blank (and not a compatible earlier attempt) - nothing was written",
    Status.VERIFY_FAILED: "the chip does not hold what was expected",
    Status.CALIB_FAILED: "calibration did not reach its target - the placeholder stays a measurement loop, "
                         "the chip can be completed later by programming it again",
    Status.CHIP_ALREADY_PROGRAMMED: "the chip already holds this package completely - nothing to do",
    Status.CANCELLED: "cancelled",
    Status.VDD_FAULT: "VDD did not settle - check the supply and the board",
    Status.VPP_FAULT: "VPP did not settle - check the supply and the board",
    Status.OP_UNSUPPORTED: "this programmer or chip does not support the operation",
}


def show_progress(pr) -> None:
    print(f"\r[{pr.stage_index + 1}/{pr.stage_count}] {stage_name(pr.stage):<12} {pr.percent:3d}%",
          end="", file=sys.stderr, flush=True)


def run_operation(prog: Programmer, a, op_type: int, flags: int = 0, chip_id12: int = 0) -> Result:
    """Runs one operation with a progress line; Ctrl+C asks the programmer to cancel it safely."""
    op_id = prog.start_op(op_type, flags, chip_id12)
    progress = None if getattr(a, "quiet", False) else show_progress
    try:
        while True:
            try:
                return prog.wait_op(op_id, progress)
            except KeyboardInterrupt:
                print("\nCancelling...", file=sys.stderr)
                try:
                    prog.cancel_op(op_id)
                except ProgrammerError:
                    pass   # already finished
    finally:
        if progress:
            print(file=sys.stderr)


def describe_result(r: Result) -> str:
    text = f"{status_name(r.status)} at {stage_name(r.stage)}"
    msg = OP_MESSAGES.get(r.status) or PKG_MESSAGES.get(r.status)
    if msg:
        text += f": {msg}"
    if r.status == Status.CHIP_ID_MISMATCH:
        text += f" (found 0x{r.chip_id_found:03X})"
    if r.status in (Status.VDD_FAULT, Status.VPP_FAULT) and r.v_rail in (0, 1):
        text += f" ({RAILS[r.v_rail]} set {r.v_set_mV / 1000:.2f} V, measured {r.v_meas_mV / 1000:.2f} V)"
    if r.fail_addr != 0xFFFF and r.status in (Status.CHIP_NOT_BLANK, Status.VERIFY_FAILED):
        text += f" (word 0x{r.fail_addr:04X}: expected 0x{r.expected:04X}, found 0x{r.actual:04X})"
    return text


def print_result(r: Result) -> None:
    for c in r.calib:
        kind = CALIB_KINDS.get(c.kind, str(c.kind))
        target = f"{c.target_hz} Hz" if c.kind != 3 else "BandGap"
        measured = f", measured {c.measured_hz} Hz" if c.measured_hz else ""
        print(f"Calibration {kind}: target {target}, value 0x{c.value:02X}{measured} "
              f"[{SITE_STATUS.get(c.site_status, c.site_status)}]")
    if r.serial_valid:
        print(f"Serial number: 0x{r.serial:016X}")
    if r.fuse_written != 0xFFFF:
        print(f"Fuse: 0x{r.fuse_written:04X}")
    if r.image_crc_raw or r.image_crc_norm:
        print(f"Chip content CRC 0x{r.image_crc_raw:08X}, normalized 0x{r.image_crc_norm:08X}")
    if r.warnings:
        print(f"Warnings: {' '.join(r.warning_names())}")
    print(f"Time: {r.elapsed_ms / 1000:.1f} s")


def finish(r: Result, done: str) -> int:
    print_result(r)
    if r.ok:
        print(done)
        return 0
    if r.status == Status.CHIP_ALREADY_PROGRAMMED:
        print(OP_MESSAGES[r.status])
        return 0
    print(f"error: {describe_result(r)}", file=sys.stderr)
    return 1


def program_flags(a) -> int:
    flags = 0
    if getattr(a, "noerase", False):
        flags |= OPF_SKIP_ERASE
    if getattr(a, "noblankchk", False):
        flags |= OPF_SKIP_BLANK_CHECK
    if getattr(a, "noverify", False):
        flags |= OPF_SKIP_VERIFY
    if a.finalize_on_calib_fail:
        flags |= OPF_CALIB_FINALIZE_ON_FAIL
    return flags


def cmd_program(prog: Programmer, a) -> int:
    r = run_operation(prog, a, Op.PROGRAM, program_flags(a))
    return finish(r, "Programming done" + (" (an earlier attempt completed)" if r.warnings & 8 else ""))


def cmd_write(prog: Programmer, a) -> int:
    rc = cmd_upload(prog, a)
    if rc:
        return rc
    a.noerase = a.noblankchk = a.noverify = False   # already in the package flags
    return cmd_program(prog, a)


def cmd_verify(prog: Programmer, a) -> int:
    return finish(run_operation(prog, a, Op.VERIFY), "Verify OK")


def cmd_probe(prog: Programmer, a) -> int:
    r = run_operation(prog, a, Op.PROBE)
    if not r.ok:
        print(f"error: {describe_result(r)}", file=sys.stderr)
        return 1
    chip = next((c for c in prog.list_chips() if c.id12 == r.chip_id_found), None)
    name = "/".join(chip.names) if chip else "unknown"
    print(f"Found: {name} (ID 0x{r.chip_id_found:03X}, family {FAMILY_NAMES.get(r.family_found, r.family_found)})")
    return 0


def chip_id_of(prog: Programmer, a) -> int:
    return prog.find_chip(a.icname).id12 if a.icname else 0


def cmd_erase(prog: Programmer, a) -> int:
    return finish(run_operation(prog, a, Op.ERASE, chip_id12=chip_id_of(prog, a)), "Erased and blank")


def cmd_blankcheck(prog: Programmer, a) -> int:
    return finish(run_operation(prog, a, Op.BLANK_CHECK, chip_id12=chip_id_of(prog, a)), "Chip is blank")


def cmd_status(prog: Programmer, a) -> int:
    s = prog.get_status()
    states = {0: "idle", 1: "receiving a package", 2: "operation running", 3: "running the chip"}
    print(f"State: {states.get(s.state, s.state)}")
    if s.op_id:
        try:
            op = Op(s.op_type).name
        except ValueError:
            op = f"0x{s.op_type:02X}"
        print(f"Operation #{s.op_id}: {op}, {stage_name(s.op_stage)}, {s.op_percent}%")
    print(f"Package: {PkgState(s.pkg_state).name}" + (f", CRC 0x{s.pkg_crc32:08X}" if s.pkg_state else ""))
    print(f"Chips programmed: {s.ok_count} ok, {s.fail_count} failed")
    if s.state != 2 and s.op_id:
        r = prog.op_result(0)
        if r:
            print(f"Last result: {status_name(r.status)} at {stage_name(r.stage)}")
    return 0


def show_supplies(s: dict) -> None:
    """One line per rail: set point, average reading, its deviation and the spread (min..max) of the samples."""
    for rail in RAILS:
        key = rail.lower()
        set_mV, avg, lo, hi = (s[f"{key}_{f}_mV"] for f in ("set", "meas", "min", "max"))
        dev = f"{avg - set_mV:+5d} mV" if set_mV else "  (off) "
        print(f"{rail} set {set_mV / 1000:6.3f} V  reads {avg / 1000:6.3f} V {dev}  "
              f"spread {lo / 1000:.3f}..{hi / 1000:.3f} V ({hi - lo} mV)")


def apply_supplies(prog: Programmer, vdd_mV: int, vpp_mV: int, check: bool = True) -> bool:
    """Applies the voltages and prints them; a refusal is printed and gives False (supplies are off then)."""
    try:
        show_supplies(prog.supply_set(vdd_mV, vpp_mV, check))
        return True
    except ProgrammerError as e:
        if e.status in (Status.VDD_FAULT, Status.VPP_FAULT) and len(e.payload) >= 5:
            rail, set_mV, meas_mV = struct.unpack_from("<BHH", e.payload)
            print(f"{RAILS[rail]} did not settle within tolerance: set {set_mV / 1000:.3f} V, "
                  f"measured {meas_mV / 1000:.3f} V. All supplies are off.")
            if not prog.cal_get()["valid"]:
                print("The supplies are not calibrated - run 'pdkprog calibrate' first.")
            return False
        if e.status == Status.BAD_PAYLOAD and e.payload and e.payload[0] < len(RAILS):
            print(f"{RAILS[e.payload[0]]} is above the limit of the programmer.")
            return False
        raise


def parse_supplies(text: str) -> tuple[int, int]:
    """'3.3' or '5 12,5' → (VDD mV, VPP mV)."""
    words = text.replace(";", " ").split()
    if not 1 <= len(words) <= 2:
        raise ValueError(text)
    vdd = parse_volts(words[0], allow_zero=True)
    vpp = parse_volts(words[1], allow_zero=True) if len(words) == 2 else 0
    return vdd, vpp


def cmd_supply(prog: Programmer, a) -> int:
    if not prog.info.capabilities & CAP_SUPPLY_SET:
        raise CliError("this programmer cannot set the supplies manually")
    print("WARNING: disconnect the programmer from any target board: the voltages go to the output pins.")
    if not a.yes and ask("Nothing is connected to the output pins? [y/N] ").strip().lower() not in ("y", "yes"):
        print("Cancelled.")
        return 1
    if a.no_check:
        print("Tolerance check off: the voltages stay up even if they are wrong - compare them with a meter.")
    try:
        if a.vdd is not None:   # one setting, held for --hold seconds (or until Enter)
            try:
                vdd = parse_volts(a.vdd, allow_zero=True)
                vpp = parse_volts(a.vpp, allow_zero=True) if a.vpp is not None else 0
            except ValueError:
                raise CliError("voltages must be numbers in volts") from None
            if not apply_supplies(prog, vdd, vpp, not a.no_check):
                return 1
            if a.hold is None:
                ask("Press Enter to switch the supplies off. ")
            else:
                end = time.monotonic() + a.hold
                while time.monotonic() < end:
                    time.sleep(min(1.0, max(0.0, end - time.monotonic())))
                    show_supplies(prog.supply_get())
            return 0

        print("Enter VDD [VPP] in volts (e.g. '3.3' or '5 12.5'; VPP 0 when omitted), 'm' to measure again, "
              "Enter to switch off and quit. Supplies drop after 5 minutes without commands.")
        while True:
            line = ask("VDD [VPP]> ").strip()
            if not line:
                return 0
            if line.lower() in ("m", "measure"):
                show_supplies(prog.supply_get())
                continue
            try:
                vdd, vpp = parse_supplies(line)
            except ValueError:
                print("Please enter one or two numbers in volts, e.g. 3.3 or 5 12.5")
                continue
            apply_supplies(prog, vdd, vpp, not a.no_check)
    finally:
        try:
            prog.supply_set(0, 0)
            print("Supplies off.")
        except Exception:
            pass   # the session is gone: the programmer switches them off by itself


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

    w = sub.add_parser("write", help="store the package and program the chip (upload + program)")
    for action in u._actions[1:]:   # the same options as upload
        w._add_action(action)
    w.add_argument("--finalize-on-calib-fail", action="store_true",
                   help="finish a calibration that missed its target with the best value found")
    w.add_argument("-q", "--quiet", action="store_true", help="no progress line")
    w.set_defaults(func=cmd_write)

    g = sub.add_parser("program", help="program the chip with the stored package")
    g.add_argument("--noerase", action="store_true")
    g.add_argument("--noblankchk", action="store_true", help="FLASH chips only")
    g.add_argument("--noverify", action="store_true", help="skip VERIFY (FINAL_VERIFY always runs)")
    g.add_argument("--finalize-on-calib-fail", action="store_true",
                   help="finish a calibration that missed its target with the best value found")
    g.add_argument("-q", "--quiet", action="store_true", help="no progress line")
    g.set_defaults(func=cmd_program)

    for name, func, text in (("verify", cmd_verify, "compare the chip with the stored package"),
                             ("probe", cmd_probe, "identify the chip in the socket")):
        q = sub.add_parser(name, help=text)
        q.add_argument("-q", "--quiet", action="store_true", help="no progress line")
        q.set_defaults(func=func)
    for name, func, text in (("erase", cmd_erase, "erase a FLASH chip"),
                             ("blankcheck", cmd_blankcheck, "check that the chip is blank")):
        q = sub.add_parser(name, help=text)
        q.add_argument("-n", "--icname", help="chip type (default: the stored package's)")
        q.add_argument("-q", "--quiet", action="store_true", help="no progress line")
        q.set_defaults(func=func)
    sub.add_parser("status", help="programmer state and the last operation").set_defaults(func=cmd_status)

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

    s = sub.add_parser("supply", help="apply arbitrary VDD/VPP for checking the supplies (nothing connected!)")
    s.add_argument("vdd", nargs="?", help="VDD in volts; without it the voltages are asked interactively")
    s.add_argument("vpp", nargs="?", help="VPP in volts (default 0 = off)")
    s.add_argument("--hold", type=float, metavar="SECONDS",
                   help="keep the voltages for SECONDS, printing readings every second (default: until Enter)")
    s.add_argument("--no-check", action="store_true",
                   help="keep the voltages up even outside the +-2 %% tolerance (diagnostics with a meter)")
    s.add_argument("--yes", action="store_true", help="do not ask whether the outputs are disconnected")
    s.set_defaults(func=cmd_supply)

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
