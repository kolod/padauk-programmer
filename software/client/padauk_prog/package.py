# SPDX-License-Identifier: GPL-3.0-or-later
"""Firmware package (contracts/package-format.md, format version 1)."""
from __future__ import annotations

import os
import struct
import time
import zlib
from dataclasses import dataclass

MAGIC = b"PDKP"
FORMAT_VERSION = 1
HEADER_SIZE = 128

FUSE_PRESENT = 1 << 0
ALLOW_SECURITY_FUSE = 1 << 1
SKIP_ERASE = 1 << 2
SKIP_BLANK_CHECK = 1 << 3
SKIP_VERIFY = 1 << 4
SECURE_FILL = 1 << 6
CREATED_BY_COPY = 1 << 8   # set by the programmer only
HOST_FLAGS = FUSE_PRESENT | ALLOW_SECURITY_FUSE | SKIP_ERASE | SKIP_BLANK_CHECK | SKIP_VERIFY | SECURE_FILL

FLAG_NAMES = {FUSE_PRESENT: "FUSE_PRESENT", ALLOW_SECURITY_FUSE: "ALLOW_SECURITY_FUSE", SKIP_ERASE: "SKIP_ERASE",
              SKIP_BLANK_CHECK: "SKIP_BLANK_CHECK", SKIP_VERIFY: "SKIP_VERIFY", SECURE_FILL: "SECURE_FILL",
              CREATED_BY_COPY: "CREATED_BY_COPY"}


class PackageError(ValueError):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def package_size(image_words: int) -> int:
    return HEADER_SIZE + 2 * image_words + 2 * ((image_words + 15) // 16) + 4


def _text(value: str, size: int, what: str, required: bool) -> bytes:
    raw = value.encode("utf-8")
    if required and not raw:
        raise PackageError(f"{what} must not be empty")
    if len(raw) > size:
        raise PackageError(f"{what} is {len(raw)} bytes in UTF-8, at most {size} allowed")
    if any(c < 0x20 or c == 0x7F for c in raw):
        raise PackageError(f"{what} contains control characters")
    return raw.ljust(size, b"\0")


def default_name(path: str) -> str:
    """Package name from a file name: base name without extension, cut to 32 UTF-8 bytes."""
    name = os.path.splitext(os.path.basename(path))[0]
    while len(name.encode("utf-8")) > 32:
        name = name[:-1]
    return name


@dataclass
class Header:
    package_size: int
    chip_id12: int
    image_words: int
    flags: int
    fuse: int
    run_vdd_mV: int
    source_crc32: int
    created_unix: int
    chip_name: str
    name: str
    label: str
    header_crc32: int

    @classmethod
    def parse(cls, raw: bytes) -> "Header":
        if len(raw) < HEADER_SIZE or raw[:4] != MAGIC:
            raise PackageError("not a package header")
        ver, hsize, size, cid, n, flags, fuse, vdd = struct.unpack_from("<HHIHHIHH", raw, 4)
        if ver != FORMAT_VERSION or hsize != HEADER_SIZE:
            raise PackageError(f"unsupported package format {ver}/{hsize}")
        src, created = struct.unpack_from("<II", raw, 40)
        (hcrc,) = struct.unpack_from("<I", raw, 124)
        if hcrc != crc32(raw[:124]):
            raise PackageError("header CRC mismatch")

        def txt(a, b):
            return raw[a:b].split(b"\0")[0].decode("utf-8", errors="replace")

        return cls(size, cid, n, flags, fuse, vdd, src, created, txt(48, 64), txt(64, 96), txt(96, 124), hcrc)

    def flag_names(self) -> list[str]:
        return [n for bit, n in FLAG_NAMES.items() if self.flags & bit]


def build(chip_id12: int, chip_name: str, words: dict[int, int], name: str, label: str = "",
          flags: int = 0, fuse: int | None = None, run_vdd_mV: int = 0, source_crc32: int = 0,
          created_unix: int | None = None, code_words: int | None = None) -> bytes:
    """Builds a package from {word_address: value}. `fuse` sets FUSE_PRESENT.

    `code_words` (from CHIP_DESC) is checked before anything is sent to the programmer.
    """
    if flags & ~HOST_FLAGS:
        raise PackageError(f"flags 0x{flags:X} contain bits a client must not set")
    if not words:
        raise PackageError("image is empty")
    if min(words) < 0:
        raise PackageError("negative address")
    n = max(words) + 1
    if code_words is not None and n > code_words:
        raise PackageError(f"image has {n} words, {chip_name} has {code_words}")
    if fuse is not None:
        flags |= FUSE_PRESENT
    if created_unix is None:
        created_unix = int(time.time())

    h = bytearray(HEADER_SIZE)
    h[0:4] = MAGIC
    struct.pack_into("<HHIHHIHH", h, 4, FORMAT_VERSION, HEADER_SIZE, package_size(n), chip_id12 & 0xFFF, n,
                     flags, (fuse or 0) & 0xFFFF, run_vdd_mV)
    struct.pack_into("<II", h, 40, source_crc32 & 0xFFFFFFFF, created_unix & 0xFFFFFFFF)
    cn = chip_name.encode("ascii")
    if not cn or len(cn) > 16:
        raise PackageError("chip name must be 1..16 ASCII characters")
    h[48:64] = cn.ljust(16, b"\0")
    h[64:96] = _text(name, 32, "package name", True)
    h[96:124] = _text(label, 28, "label", False)
    struct.pack_into("<I", h, 124, crc32(bytes(h[:124])))

    body = bytearray(h)
    mask = bytearray(2 * ((n + 15) // 16))
    for a in range(n):
        w = words.get(a)
        body += struct.pack("<H", 0xFFFF if w is None else w & 0xFFFF)
        if w is not None:
            mask[a // 8] |= 1 << (a % 8)
    body += mask
    body += struct.pack("<I", crc32(bytes(body)))
    return bytes(body)


def check(pkg: bytes) -> Header:
    """Parses a whole package and checks its size and trailer CRC."""
    h = Header.parse(pkg)
    if len(pkg) != h.package_size or h.package_size != package_size(h.image_words):
        raise PackageError("package size mismatch")
    if struct.unpack_from("<I", pkg, len(pkg) - 4)[0] != crc32(pkg[:-4]):
        raise PackageError("package CRC mismatch")
    return h


def words_of(pkg: bytes) -> dict[int, int]:
    h = check(pkg)
    n = h.image_words
    mask = pkg[HEADER_SIZE + 2 * n:]
    return {a: struct.unpack_from("<H", pkg, HEADER_SIZE + 2 * a)[0]
            for a in range(n) if mask[a // 8] >> (a % 8) & 1}


def trailer_crc(pkg: bytes) -> int:
    return struct.unpack_from("<I", pkg, len(pkg) - 4)[0]
