# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate firmware-package test vectors (contracts/package-format.md).

Usage: python gen_packages.py [--out DIR] [--c-header PATH]

Writes DIR/<name>.bin for each vector and DIR/manifest.json with the expected outcome:
  stage  "begin"  — rejected by PKG_BEGIN (header only), "commit" — by PKG_COMMIT, "ok" — accepted;
  status — numeric status code, addr — the `addr` of the COMMIT error (0xFFFF = none);
  calib_sites / serial_sites / effective_words — PKG_COMMIT result of valid vectors.
Independent reference encoder: it must not import the client package.
"""
import argparse
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

OK = 0x0000
HEADER_INVALID = 0x0203
CHIP_UNSUPPORTED = 0x0201
TOO_LARGE = 0x0202
CRC_MISMATCH = 0x0205
RESERVED_AREA = 0x0206
FUSE_CONFLICT = 0x0207
SECURITY_NOT_ALLOWED = 0x0208
CALIB_UNKNOWN = 0x0209
CHIP_READ_ONLY = 0x020B
OTP_BLANK_CHECK_REQUIRED = 0x020C

FUSE_PRESENT, ALLOW_SECURITY_FUSE, SKIP_ERASE, SKIP_BLANK_CHECK, SKIP_VERIFY = 1, 2, 4, 8, 16
SECURE_FILL, CREATED_BY_COPY = 1 << 6, 1 << 8

# chip: (id12, code_words, code_bits, reserved_start, reserved_end_inclusive)
CHIPS = {
    "PFS154": (0xAA1, 0x800, 14, 0x7E0, 0x7F0),
    "PMS150C": (0xA16, 0x400, 13, 0x3F6, 0x400),
    "PFS173": (0xEA2, 0xC00, 15, 0xBE0, 0xBF0),
    "PMS131": (0xC83, 0x600, 14, 0x5F9, 0x600),
}
AND_OP = {13: 0x1400, 14: 0x2C00, 15: 0x5400, 16: 0x1C00}
RET_OP = {13: 0x0100, 14: 0x0200, 15: 0x0200, 16: 0x0F00}
NOP = 0x0000


def header(chip, n, flags=0, fuse=0, name=b"test", label=b"", chip_name=None, size=None,
           magic=b"PDKP", version=1, hsize=128, reserved=b"", run_vdd=0, crc_fix=True, id12=None,
           source_crc=0x12345678, created=1700000000):
    cid = CHIPS[chip][0] if id12 is None else id12
    size = (128 + 2 * n + 2 * ((n + 15) // 16) + 4) if size is None else size
    h = bytearray(128)
    h[0:4] = magic
    struct.pack_into("<HHIHHIHH", h, 4, version, hsize, size, cid, n, flags, fuse, run_vdd)
    h[24:24 + len(reserved)] = reserved
    struct.pack_into("<II", h, 40, source_crc, created)
    cn = (chip_name if chip_name is not None else chip.encode())
    h[48:48 + len(cn)] = cn
    h[64:64 + len(name)] = name
    h[96:96 + len(label)] = label
    if crc_fix:
        struct.pack_into("<I", h, 124, zlib.crc32(bytes(h[:124])) & 0xFFFFFFFF)
    return bytes(h)


def package(hdr, words: dict, n=None, bad_crc=False):
    """`words`: {addr: value}; undefined addresses are 0xFFFF with mask bit 0."""
    n = struct.unpack_from("<H", hdr, 14)[0] if n is None else n
    body = bytearray(hdr)
    for a in range(n):
        body += struct.pack("<H", words.get(a, 0xFFFF))
    mask = bytearray(2 * ((n + 15) // 16))
    for a in words:
        if a < n:
            mask[a // 8] |= 1 << (a % 8)
    body += mask
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    body += struct.pack("<I", crc ^ (1 if bad_crc else 0))
    return bytes(body)


def seq(start, values):
    return {start + i: v for i, v in enumerate(values)}


def rc_site(bits, kind, hz, mv, reg):
    imm = [ord("R"), ord("C"), kind, hz & 0xFF, hz >> 8 & 0xFF, hz >> 16 & 0xFF, hz >> 24 & 0xFF,
           mv & 0xFF, mv >> 8, reg]
    return [AND_OP[bits] | v for v in imm]


def bg_site(bits, mv, bgtr, gpcc, gpcs):
    imm = [ord("B"), ord("G"), 3, mv & 0xFF, mv >> 8, bgtr, gpcc, gpcs] + [0] * 6
    return [AND_OP[bits] | v for v in imm]


def serial_site(bits):
    return [RET_OP[bits] | ord(c) for c in "FPSERIAL"]


def ihx_words(path):
    """Intel HEX (byte addresses) → {word_addr: value}; a word with one byte given has 0xFF in the other."""
    mem, base = {}, 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            b = bytes.fromhex(line[1:])
            n, addr, typ = b[0], b[1] << 8 | b[2], b[3]
            if typ == 0:
                for i in range(n):
                    mem[base + addr + i] = b[4 + i]
            elif typ == 2:
                base = (b[4] << 8 | b[5]) << 4
            elif typ == 4:
                base = (b[4] << 8 | b[5]) << 16
            elif typ == 1:
                break
    return {a // 2: mem.get(a // 2 * 2, 0xFF) | mem.get(a // 2 * 2 + 1, 0xFF) << 8 for a in mem}


EXAMPLE_CHIPS = {"pfs154": "PFS154", "pfs172": "PFS172", "pfs173": "PFS173", "pms150c": "PMS150C",
                 "pms152": "PMS152", "pms171b": "PMS171B"}


def example_vectors(add):
    d = os.path.join(HERE, "examples")
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".ihx"):
            continue
        stem = fn[:-4]
        chip = EXAMPLE_CHIPS[stem.split("_")[1]]
        with open(os.path.join(d, fn), "rb") as f:
            src_crc = zlib.crc32(f.read()) & 0xFFFFFFFF
        words = ihx_words(os.path.join(d, fn))
        n = max(words) + 1
        hdr = header(chip, n, name=stem.encode(), source_crc=src_crc, created=0, id12=EX_IDS[chip])
        add("ex_" + stem, package(hdr, words), "ok", calib=0 if stem.startswith("adctest") else 1,
            eff=len(words), note=f"from examples/{fn}, created_unix 0")


EX_IDS = {"PFS154": 0xAA1, "PFS172": 0xCA6, "PFS173": 0xEA2, "PMS150C": 0xA16, "PMS152": 0xA27, "PMS171B": 0xD36}


def build():
    v = []

    def add(name, data, stage, status=OK, addr=0xFFFF, calib=0, serial=0, eff=0, note=""):
        v.append(dict(name=name, data=data, stage=stage, status=status, addr=addr,
                      calib_sites=calib, serial_sites=serial, effective_words=eff, note=note))

    # ---------------- valid
    # Reference example from package-format.md: PFS154, 3 words, no fuse → 140 bytes.
    add("ok_pfs154_3words", package(header("PFS154", 3, name=b"example"), seq(0, [0x0070, NOP, 0x3000])),
        "ok", eff=3, note="reference example, package_size 140")
    # helloworld-like PFS154 program with an IHRC 8 MHz placeholder at 0x002.
    hello = [0x0070, NOP] + rc_site(14, 1, 8000000, 4000, 0x0B) + [0x2F82, 0x0182, 0x3000 | 0x0C]
    add("ok_pfs154_ihrc", package(header("PFS154", len(hello), name=b"helloworld_pfs154"), seq(0, hello)),
        "ok", calib=1, eff=len(hello))
    # IHRC + BG on PFS173 (15-bit), placeholders preceded by an ordinary AND A,#0x0F.
    p173 = [0x5400 | 0x0F] + rc_site(15, 1, 16000000, 5000, 0x0B) + bg_site(15, 4000, 0x63, 0x2B, 0x2C) + [NOP]
    add("ok_pfs173_ihrc_bg", package(header("PFS173", len(p173), name="прошивка".encode()), seq(0, p173)),
        "ok", calib=2, eff=len(p173), note="UTF-8 name")
    # Serial number placeholder (RET 'FPSERIAL') on PMS150C (13-bit), OTP.
    ser = [NOP] * 4 + serial_site(13) + [NOP]
    add("ok_pms150c_serial", package(header("PMS150C", len(ser), name=b"serial"), seq(0, ser)),
        "ok", serial=1, eff=len(ser))
    # No placeholders, fuse in the header, label, words scattered (holes in the mask).
    add("ok_pfs154_nocalib_fuse",
        package(header("PFS154", 40, flags=FUSE_PRESENT, fuse=0x3FFD, name=b"plain", label=b"rev A"),
                {0: 0x0070, 1: NOP, 17: 0x3000, 39: NOP}),
        "ok", eff=4)
    # Fuse as the last image word, matching the header.
    add("ok_pfs154_fuse_word",
        package(header("PFS154", 0x800, flags=FUSE_PRESENT, fuse=0x3FFD, name=b"fuseword"),
                {0: 0x0070, 0x7FF: 0x3FFD}),
        "ok", eff=1)
    # Security fuse allowed explicitly.
    add("ok_pfs154_secure_allowed",
        package(header("PFS154", 2, flags=FUSE_PRESENT | ALLOW_SECURITY_FUSE, fuse=0x3FFC, name=b"sec"),
                seq(0, [NOP, NOP])),
        "ok", eff=2)
    # SECURE_FILL: everything up to the reserved area is written.
    add("ok_pfs154_secure_fill",
        package(header("PFS154", 3, flags=SECURE_FILL | SKIP_BLANK_CHECK | SKIP_VERIFY, name=b"fill"),
                seq(0, [0x0070, NOP, 0x3000])),
        "ok", eff=0x7E0)
    # Variant name of a chip.
    add("ok_pms15a_variant", package(header("PMS150C", 2, chip_name=b"PMS15A", name=b"variant"), seq(0, [NOP, NOP])),
        "ok", eff=2)
    # Five AND A,# in a row (below the "unknown placeholder" threshold).
    and5 = [0x2C00 | i for i in range(5)] + [NOP]
    add("ok_pfs154_and5", package(header("PFS154", len(and5), name=b"and5"), seq(0, and5)), "ok", eff=len(and5))
    example_vectors(add)

    # ---------------- rejected by PKG_BEGIN
    w3 = seq(0, [NOP, NOP, NOP])

    def bad_begin(name, hdr, status, note=""):
        add(name, package(hdr, w3, n=3) if len(hdr) == 128 else hdr, "begin", status, note=note)

    bad_begin("bad_magic", header("PFS154", 3, magic=b"PDKQ"), HEADER_INVALID)
    bad_begin("bad_version", header("PFS154", 3, version=2), HEADER_INVALID)
    bad_begin("bad_header_size", header("PFS154", 3, hsize=64), HEADER_INVALID)
    h = bytearray(header("PFS154", 3))
    h[124] ^= 1
    bad_begin("bad_header_crc", bytes(h), HEADER_INVALID)
    bad_begin("bad_flag_bit5", header("PFS154", 3, flags=1 << 5), HEADER_INVALID, "reserved bit (no calibrate)")
    bad_begin("bad_flag_bit7", header("PFS154", 3, flags=1 << 7), HEADER_INVALID, "reserved bit (serial)")
    bad_begin("bad_flag_bit31", header("PFS154", 3, flags=1 << 31), HEADER_INVALID)
    bad_begin("bad_created_by_copy", header("PFS154", 3, flags=CREATED_BY_COPY), HEADER_INVALID)
    bad_begin("bad_reserved_field", header("PFS154", 3, reserved=b"\0\0\0\0\0\0\0\x01"), HEADER_INVALID)
    bad_begin("bad_name_empty", header("PFS154", 3, name=b""), HEADER_INVALID)
    bad_begin("bad_name_control", header("PFS154", 3, name=b"a\tb"), HEADER_INVALID)
    bad_begin("bad_name_utf8", header("PFS154", 3, name=b"\xc3\x28"), HEADER_INVALID)
    bad_begin("bad_name_not_padded", header("PFS154", 3, name=b"ab\0c"), HEADER_INVALID)
    bad_begin("bad_label_utf8", header("PFS154", 3, label=b"\xff"), HEADER_INVALID)
    bad_begin("bad_chip_name_empty", header("PFS154", 3, chip_name=b""), HEADER_INVALID)
    bad_begin("bad_package_size", header("PFS154", 3, size=142), HEADER_INVALID)
    bad_begin("bad_image_words_zero", header("PFS154", 0), HEADER_INVALID)
    bad_begin("bad_chip_id", header("PFS154", 3, id12=0x123), CHIP_UNSUPPORTED)
    bad_begin("bad_chip_name_mismatch", header("PFS154", 3, chip_name=b"PFS173"), CHIP_UNSUPPORTED)
    bad_begin("bad_read_only_chip", header("PMS131", 3), CHIP_READ_ONLY)
    add("bad_too_many_words", package(header("PFS154", 0x801), {0: NOP}), "begin", TOO_LARGE)
    bad_begin("bad_otp_skip_blank_check", header("PMS150C", 3, flags=SKIP_BLANK_CHECK), OTP_BLANK_CHECK_REQUIRED)
    bad_begin("bad_security_fuse_header", header("PFS154", 3, flags=FUSE_PRESENT, fuse=0x3FFC), SECURITY_NOT_ALLOWED)

    # ---------------- rejected by PKG_COMMIT
    add("bad_trailer_crc", package(header("PFS154", 3), w3, bad_crc=True), "commit", CRC_MISMATCH, 0xFFFF)
    add("bad_reserved_area", package(header("PFS154", 0x7E1), {0: NOP, 0x7E0: NOP}), "commit", RESERVED_AREA, 0x7E0)
    add("bad_reserved_area_end", package(header("PFS154", 0x7F1), {0x7F0: NOP}), "commit", RESERVED_AREA, 0x7F0)
    add("bad_fuse_conflict",
        package(header("PFS154", 0x800, flags=FUSE_PRESENT, fuse=0x3FFD), {0x7FF: 0x3FFF}),
        "commit", FUSE_CONFLICT, 0x7FF)
    add("bad_security_fuse_word", package(header("PFS154", 0x800), {0x7FF: 0x3FFE}),
        "commit", SECURITY_NOT_ALLOWED, 0x7FF)
    unk = [NOP, NOP] + [0x2C00 | 0x41] * 6
    add("bad_calib_unknown", package(header("PFS154", len(unk)), seq(0, unk)), "commit", CALIB_UNKNOWN, 2)
    # An unknown run right before a real placeholder still fails.
    unk2 = [0x2C00 | 0x41] * 6 + rc_site(14, 1, 8000000, 4000, 0x0B)
    add("bad_calib_unknown_before_site", package(header("PFS154", len(unk2)), seq(0, unk2)), "commit", CALIB_UNKNOWN, 0)
    return v


def c_array(name, data):
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def write_header(v, path):
    out = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
           "/* Generated by software/tests/vectors/gen_packages.py — do not edit. */",
           "#ifndef VECTORS_PACKAGES_H", "#define VECTORS_PACKAGES_H", "#include <stddef.h>", "#include <stdint.h>", "",
           "typedef enum { PV_OK, PV_BEGIN, PV_COMMIT } pv_stage_t;", "",
           "typedef struct {", "    const char *name;", "    const uint8_t *data;", "    size_t len;",
           "    pv_stage_t stage;", "    uint16_t status;", "    uint16_t addr;", "    uint8_t calib_sites;",
           "    uint8_t serial_sites;", "    uint16_t effective_words;", "} pkg_vector_t;", ""]
    for e in v:
        out.append(c_array("pv_" + e["name"], e["data"]))
    stage = {"ok": "PV_OK", "begin": "PV_BEGIN", "commit": "PV_COMMIT"}
    out.append("static const pkg_vector_t pkg_vectors[] = {")
    for e in v:
        out.append(f'    {{"{e["name"]}", pv_{e["name"]}, sizeof pv_{e["name"]}, {stage[e["stage"]]}, '
                   f'0x{e["status"]:04X}, 0x{e["addr"]:04X}, {e["calib_sites"]}, {e["serial_sites"]}, '
                   f'{e["effective_words"]}}},')
    out += ["};", "#define PKG_VECTOR_COUNT (sizeof pkg_vectors / sizeof pkg_vectors[0])", "", "#endif", ""]
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "packages"))
    ap.add_argument("--c-header")
    args = ap.parse_args()
    v = build()
    os.makedirs(args.out, exist_ok=True)
    manifest = []
    for e in v:
        fn = e["name"] + ".bin"
        with open(os.path.join(args.out, fn), "wb") as f:
            f.write(e["data"])
        m = {k: e[k] for k in e if k != "data"}
        m["file"] = fn
        m["crc32"] = struct.unpack_from("<I", e["data"], len(e["data"]) - 4)[0]
        manifest.append(m)
    with open(os.path.join(args.out, "manifest.json"), "w", newline="\n", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
        f.write("\n")
    if args.c_header:
        write_header(v, args.c_header)


if __name__ == "__main__":
    main()
