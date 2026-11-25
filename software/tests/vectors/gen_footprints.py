# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate calibration footprint / measurement loop / image checksum test vectors (research R8, R9).

Usage: python gen_footprints.py [--out DIR] [--c-header PATH]

Writes DIR/manifest.json and a C header with:
  * loops   — placeholder → measurement loop → finished form for every core width and kind, at a given
              position (the loop/finish opcodes are ported from easy-pdk-programmer-software/fpdkiccalib.c,
              Copyright (C) 2019-2020 freepdk https://free-pdk.github.io, GPL-3.0-or-later);
  * dumps   — whole calibrated chip dumps (13/14/15 bit) with finished footprints, measurement loops with
              an arbitrary `MOV A,#…`, footprints with a foreign register address, a trim register write
              outside a footprint, a written serial number and a longer RET table; expected matches,
              restored placeholders and image_crc_raw / image_crc_norm.
Independent reference: it must not import the firmware or the client.
"""
import argparse
import json
import os
import random
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

IHRC, ILRC, BG = 1, 2, 3
FOOTPRINT, LOOP = 1, 2

AND_OP = {13: 0x1400, 14: 0x2C00, 15: 0x5400}
MOV_A_IMM = {13: 0x1700, 14: 0x2F00, 15: 0x5700}
MOV_IO_A = {13: 0x0080, 14: 0x0180, 15: 0x0100}
RET_IMM = {13: 0x0100, 14: 0x0200, 15: 0x0200}

# fpdkiccalib.c: (calibration opcode, flags) per word; flags: F = add position (GOTO fixup),
# R0/R1/R2 = OR register 0/1/2, N = NOP after calibration, S = immediate := result after calibration
RC_LOOP = {
    13: [(0x0F71, "N"), (0x0F8D, "N"), (0x17FF, "S"), (0x1001, "N"), (0x0F70, "N"), (0x0080, "R0"),
         (0x0C90, "N"), (0x1001, "N"), (0x0E70, "N"), (0x1804, "FN")],
    14: [(0x1ED1, "N"), (0x1F0D, "N"), (0x2FFF, "S"), (0x2801, "N"), (0x1ED0, "N"), (0x0180, "R0"),
         (0x1910, "N"), (0x2801, "N"), (0x1CD0, "N"), (0x3004, "FN")],
    15: [(0x3D91, "N"), (0x3E0D, "N"), (0x57FF, "S"), (0x5001, "N"), (0x3D90, "N"), (0x0100, "R0"),
         (0x3210, "N"), (0x5001, "N"), (0x3990, "N"), (0x6004, "FN")],
}
BG_LOOP = {
    13: [(0x1709, "N"), (0x0091, "N"), (0x0F8D, "N"), (0x1784, "N"), (0x0080, "R1N"), (0x17A3, "N"),
         (0x0080, "R2N"), (0x17FF, "S"), (0x0F70, "N"), (0x0080, "R0"), (0x0C90, "N"), (0x1001, "N"),
         (0x0E70, "N"), (0x1808, "FN")],
    14: [(0x2F09, "N"), (0x0191, "N"), (0x1F0D, "N"), (0x2F84, "N"), (0x0180, "R1N"), (0x2FA3, "N"),
         (0x0180, "R2N"), (0x2FFF, "S"), (0x1ED0, "N"), (0x0180, "R0"), (0x1910, "N"), (0x2801, "N"),
         (0x1CD0, "N"), (0x3008, "FN")],
    15: [(0x5709, "N"), (0x0111, "N"), (0x3E0D, "N"), (0x5784, "N"), (0x0100, "R1N"), (0x57A3, "N"),
         (0x0100, "R2N"), (0x57FF, "S"), (0x3D90, "N"), (0x0100, "R0"), (0x3210, "N"), (0x5001, "N"),
         (0x3990, "N"), (0x6008, "FN")],
}

# chip: code_bits, code_words, reserved (start, end inclusive), excl_first, IHRCR, ILRCR, BGTR, GPCC, GPCS
CHIPS = {
    "PMS150C": dict(bits=13, words=0x400, res=(0x3F6, 0x400), excl_first=False,
                    regs=dict(ihrcr=0x0B, ilrcr=0x1F, bgtr=0x19, gpcc=0x1A, gpcs=0x1E)),
    "PFS154": dict(bits=14, words=0x800, res=(0x7E0, 0x7F0), excl_first=False,
                   regs=dict(ihrcr=0x0B, ilrcr=0x39, bgtr=0x1A, gpcc=0x18, gpcs=0x19)),
    "PFS173": dict(bits=15, words=0xC00, res=(0xBE0, 0xBF0), excl_first=False,
                   regs=dict(ihrcr=0x0B, ilrcr=0x62, bgtr=0x63, gpcc=0x2B, gpcs=0x2C)),
}


def placeholder(bits, kind, target_hz, mv, reg0, reg1=0, reg2=0):
    a = AND_OP[bits]
    if kind == BG:
        vals = [ord("B"), ord("G"), BG, mv & 0xFF, mv >> 8, reg0, reg1, reg2] + [0] * 6
    else:
        vals = [ord("R"), ord("C"), kind] + list(target_hz.to_bytes(4, "little")) + [mv & 0xFF, mv >> 8, reg0]
    return [a | v for v in vals]


def loop(bits, kind, pos, regs, imm=0xFF):
    """Measurement loop as free-pdk inserts it; `imm` replaces the 0xFF of the `S` word."""
    out = []
    for copc, fl in (BG_LOOP if kind == BG else RC_LOOP)[bits]:
        w = copc
        for i in range(3):
            if f"R{i}" in fl:
                w |= regs[i]
        if "F" in fl:
            w += pos
        if "S" in fl:
            w = (w & 0xFF00) | imm
        out.append(w)
    return out


def finish(bits, kind, loop_words, k):
    out = []
    for w, (_, fl) in zip(loop_words, (BG_LOOP if kind == BG else RC_LOOP)[bits]):
        if "S" in fl:
            w = (w & 0xFF00) | k
        if "N" in fl:
            w = 0
        out.append(w)
    return out


def footprint(bits, kind, reg, k):
    if kind == BG:
        w = [0] * 14
        w[7], w[9] = MOV_A_IMM[bits] | k, MOV_IO_A[bits] | reg
    else:
        w = [0] * 10
        w[2], w[5] = MOV_A_IMM[bits] | k, MOV_IO_A[bits] | reg
    return w


def site_regs(chip, kind):
    r = CHIPS[chip]["regs"]
    if kind == BG:
        return [r["bgtr"], r["gpcc"], r["gpcs"]]
    return [r["ihrcr"] if kind == IHRC else r["ilrcr"], 0, 0]


# ---------------------------------------------------------------- reference matcher and checksums

def match_at(chip, w, pos):
    """(form, kind, k, length) of a footprint/loop starting at w[pos], or None."""
    c = CHIPS[chip]
    bits, r = c["bits"], c["regs"]
    for kind, n in ((IHRC, 10), (ILRC, 10), (BG, 14)):
        if pos + n > len(w):
            continue
        seg = w[pos:pos + n]
        regs = site_regs(chip, kind)
        kslot = 7 if kind == BG else 2
        k = seg[kslot] & 0xFF
        if seg == footprint(bits, kind, regs[0], k):
            return (FOOTPRINT, kind, k, n)
        if seg == loop(bits, kind, pos, regs, k):
            return (LOOP, kind, k, n)
    return None


def scan(chip, w):
    out, a = [], 0
    while a < len(w):
        m = match_at(chip, w, a)
        if m:
            out.append(dict(addr=a, form=m[0], kind=m[1], k=m[2], len=m[3]))
            a += m[3]
        else:
            a += 1
    return out


def is_ret(bits, x):
    return (x & 0xFF00) == RET_IMM[bits]


def normalize(chip, w):
    """k of finished footprints → 0, imm of serial sites (exactly 8 RET #imm in a row) → 0."""
    c = CHIPS[chip]
    bits = c["bits"]
    w = list(w)
    for m in scan(chip, w):
        if m["form"] == FOOTPRINT:
            kslot = m["addr"] + (7 if m["kind"] == BG else 2)
            w[kslot] &= 0xFF00
    a = 0
    while a < len(w):
        if not is_ret(bits, w[a]):
            a += 1
            continue
        b = a
        while b < len(w) and is_ret(bits, w[b]):
            b += 1
        if b - a == 8:
            for i in range(a, b):
                w[i] &= 0xFF00
        a = b
    return w


def image_crc(chip, w):
    c = CHIPS[chip]
    mask = (1 << c["bits"]) - 1
    rs, re_ = c["res"]
    fuse = c["words"] - 1
    data = bytearray()
    for a, x in enumerate(w):
        if c["excl_first"] and a == 0:
            continue
        if a != fuse and rs <= a <= re_:
            continue
        data += (x & mask).to_bytes(2, "little")
    return zlib.crc32(bytes(data))


# ---------------------------------------------------------------- vectors

def loop_vectors():
    out = []
    for bits, chip in ((13, "PMS150C"), (14, "PFS154"), (15, "PFS173")):
        for kind, pos, k in ((IHRC, 0x123, 0x5A), (ILRC, 0x011, 0x80), (BG, 0x2F0, 0xC3)):
            regs = site_regs(chip, kind)
            hz = {IHRC: 8000000, ILRC: 62500, BG: 0}[kind]
            mv = 3950 if kind == BG else 4000
            lp = loop(bits, kind, pos, regs)
            out.append(dict(bits=bits, chip=chip, kind=kind, pos=pos, target_hz=hz, mv=mv, regs=regs, k=k,
                            placeholder=placeholder(bits, kind, hz, mv, *regs), loop=lp,
                            final=finish(bits, kind, lp, k)))
    return out


def dump_vector(chip, seed):
    c = CHIPS[chip]
    bits, mask = c["bits"], (1 << c["bits"]) - 1
    rnd = random.Random(seed)
    # plain "code": random words that are neither AND A,#, MOV A,#, RET #imm nor NOP
    def plain():
        while True:
            x = rnd.randrange(1, mask + 1)
            if (x & 0xFF00) not in (AND_OP[bits], MOV_A_IMM[bits], RET_IMM[bits]) and x != 0:
                return x
    w = [plain() for _ in range(c["words"])]
    r = c["regs"]
    expect, restore = [], []

    def put(addr, words):
        w[addr:addr + len(words)] = words

    put(0x020, footprint(bits, IHRC, r["ihrcr"], 0x5A))
    expect.append(dict(addr=0x020, form=FOOTPRINT, kind=IHRC, k=0x5A, len=10))
    restore.append(dict(addr=0x020, kind=IHRC, target_hz=8000000, mv=4000,
                        words=placeholder(bits, IHRC, 8000000, 4000, r["ihrcr"])))
    put(0x040, footprint(bits, ILRC, r["ilrcr"], 0x33))
    expect.append(dict(addr=0x040, form=FOOTPRINT, kind=ILRC, k=0x33, len=10))
    restore.append(dict(addr=0x040, kind=ILRC, target_hz=0xFF000033, mv=5000,
                        words=placeholder(bits, ILRC, 0xFF000033, 5000, r["ilrcr"])))
    put(0x060, footprint(bits, BG, r["bgtr"], 0x77))
    expect.append(dict(addr=0x060, form=FOOTPRINT, kind=BG, k=0x77, len=14))
    restore.append(dict(addr=0x060, kind=BG, target_hz=0, mv=3950,
                        words=placeholder(bits, BG, 0, 3950, r["bgtr"], r["gpcc"], r["gpcs"])))
    put(0x080, loop(bits, IHRC, 0x080, site_regs(chip, IHRC), 0x41))          # arbitrary MOV A,#…
    expect.append(dict(addr=0x080, form=LOOP, kind=IHRC, k=0x41, len=10))
    put(0x0A0, loop(bits, BG, 0x0A0, site_regs(chip, BG)))
    expect.append(dict(addr=0x0A0, form=LOOP, kind=BG, k=0xFF, len=14))
    put(0x0C0, footprint(bits, IHRC, 0x05, 0x12))                              # foreign register: no match
    put(0x0E0, [MOV_A_IMM[bits] | 0x44, MOV_IO_A[bits] | r["ihrcr"]])          # trim write outside a footprint
    put(0x0E8, loop(bits, IHRC, 0x0E9, site_regs(chip, IHRC), 0x10))           # GOTO does not match position
    put(0x100, [RET_IMM[bits] | rnd.randrange(256) for _ in range(8)])         # written serial number
    put(0x120, [RET_IMM[bits] | rnd.randrange(256) for _ in range(9)])         # RET table: not a serial site
    w[c["words"] - 1] = mask                                                   # fuse word, no protection
    norm = normalize(chip, w)
    assert scan(chip, w) == expect, (chip, scan(chip, w))
    return dict(name=f"dump_{chip.lower()}", chip=chip, bits=bits, words=w, matches=expect, restore=restore,
                image_crc_raw=image_crc(chip, w), image_crc_norm=image_crc(chip, norm), serial_addr=0x100)


def c_array(name, words, ctype="uint16_t"):
    lines = [f"static const {ctype} {name}[{len(words)}] = {{"]
    for i in range(0, len(words), 12):
        lines.append("    " + ", ".join(f"0x{x:04X}" for x in words[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_header(path, loops, dumps):
    out = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
           "/* Generated by software/tests/vectors/gen_footprints.py — do not edit. */",
           "#ifndef VECTORS_FOOTPRINTS_H", "#define VECTORS_FOOTPRINTS_H", "#include <stdint.h>", "",
           "typedef struct {", "    uint8_t bits, kind;", "    const char *chip;", "    uint16_t pos;",
           "    uint32_t target_hz;", "    uint16_t mv;", "    uint8_t regs[3], k;",
           "    uint16_t placeholder[14], loop[14], final[14];", "} fp_loop_vector_t;", "",
           "typedef struct { uint16_t addr; uint8_t form, kind, k, len; } fp_match_t;",
           "typedef struct { uint16_t addr; uint8_t kind; uint32_t target_hz; uint16_t mv; uint16_t words[14]; } fp_restore_t;",
           "typedef struct {", "    const char *chip;", "    const uint16_t *words;", "    uint16_t count;",
           "    const fp_match_t *matches;", "    uint8_t n_matches;", "    const fp_restore_t *restore;",
           "    uint8_t n_restore;", "    uint32_t image_crc_raw, image_crc_norm;", "    uint16_t serial_addr;",
           "} fp_dump_vector_t;", ""]

    def arr14(xs):
        return "{" + ", ".join(f"0x{x:04X}" for x in xs + [0] * (14 - len(xs))) + "}"

    out.append("static const fp_loop_vector_t fp_loops[] = {")
    for v in loops:
        out.append(f"    {{{v['bits']}, {v['kind']}, \"{v['chip']}\", 0x{v['pos']:04X}, {v['target_hz']}u, {v['mv']}, "
                   f"{{0x{v['regs'][0]:02X}, 0x{v['regs'][1]:02X}, 0x{v['regs'][2]:02X}}}, 0x{v['k']:02X},")
        out.append(f"     {arr14(v['placeholder'])},")
        out.append(f"     {arr14(v['loop'])},")
        out.append(f"     {arr14(v['final'])}}},")
    out.append("};\n")
    for d in dumps:
        n = d["name"]
        out.append(c_array(f"{n}_words", d["words"]))
        out.append(f"static const fp_match_t {n}_matches[] = {{")
        out += [f"    {{0x{m['addr']:04X}, {m['form']}, {m['kind']}, 0x{m['k']:02X}, {m['len']}}}," for m in d["matches"]]
        out.append("};")
        out.append(f"static const fp_restore_t {n}_restore[] = {{")
        out += [f"    {{0x{r['addr']:04X}, {r['kind']}, 0x{r['target_hz']:08X}u, {r['mv']}, {arr14(r['words'])}}},"
                for r in d["restore"]]
        out.append("};\n")
    out.append("static const fp_dump_vector_t fp_dumps[] = {")
    for d in dumps:
        n = d["name"]
        out.append(f"    {{\"{d['chip']}\", {n}_words, {len(d['words'])}, {n}_matches, {len(d['matches'])}, "
                   f"{n}_restore, {len(d['restore'])}, 0x{d['image_crc_raw']:08X}u, 0x{d['image_crc_norm']:08X}u, "
                   f"0x{d['serial_addr']:04X}}},")
    out += ["};", "", "#endif", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "footprints"))
    ap.add_argument("--c-header", default=os.path.join(HERE, "..", "..", "firmware", "tests", "vectors_footprints.h"))
    a = ap.parse_args()
    loops = loop_vectors()
    for v in loops:   # every loop → finished transition only clears bits
        assert all(f & l == f for f, l in zip(v["final"], v["loop"])), v
    dumps = [dump_vector(chip, seed) for seed, chip in enumerate(CHIPS)]
    os.makedirs(a.out, exist_ok=True)
    manifest = dict(loops=loops, dumps=[{k: v for k, v in d.items() if k != "words"} for d in dumps])
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    for d in dumps:
        with open(os.path.join(a.out, d["name"] + ".bin"), "wb") as f:
            f.write(b"".join(x.to_bytes(2, "little") for x in d["words"]))
    write_header(a.c_header, loops, dumps)


if __name__ == "__main__":
    main()
