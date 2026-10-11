# SPDX-License-Identifier: GPL-3.0-or-later
"""Intel HEX parsing and package building against tests/vectors/packages (built independently)."""
import json
import os
import zlib

import pytest

from padauk_prog import ihex, package


@pytest.fixture(scope="module")
def manifest(vectors_dir):
    with open(os.path.join(vectors_dir, "packages", "manifest.json"), encoding="utf-8") as f:
        return json.load(f)


def load(vectors_dir, entry):
    with open(os.path.join(vectors_dir, "packages", entry["file"]), "rb") as f:
        return f.read()


def test_examples_build_bitwise_identical(vectors_dir, manifest):
    examples = [e for e in manifest if e["name"].startswith("ex_")]
    assert len(examples) == 13
    for e in examples:
        ref = load(vectors_dir, e)
        h = package.Header.parse(ref)   # chip id/name as the programmer's LIST_CHIPS would give them
        path = os.path.join(vectors_dir, "examples", e["name"][3:] + ".ihx")
        with open(path, "rb") as f:
            src_crc = zlib.crc32(f.read())
        pkg = package.build(h.chip_id12, h.chip_name, ihex.read_words(path), package.default_name(path),
                            source_crc32=src_crc, created_unix=0)
        assert pkg == ref, e["name"]


def test_valid_vectors_round_trip(vectors_dir, manifest):
    for e in manifest:
        if e["stage"] != "ok":
            continue
        ref = load(vectors_dir, e)
        h = package.check(ref)
        fuse = h.fuse if h.flags & package.FUSE_PRESENT else None
        pkg = package.build(h.chip_id12, h.chip_name, package.words_of(ref), h.name, h.label,
                            flags=h.flags & ~package.FUSE_PRESENT, fuse=fuse, run_vdd_mV=h.run_vdd_mV,
                            source_crc32=h.source_crc32, created_unix=h.created_unix)
        assert pkg == ref, e["name"]


def test_reference_example_size():
    pkg = package.build(0xAA1, "PFS154", {0: 0x0070, 1: 0, 2: 0x3000}, "example", created_unix=0)
    assert len(pkg) == 140
    assert pkg[128 + 6:128 + 8] == b"\x07\x00"
    assert package.check(pkg).image_words == 3


def test_client_refuses_bad_input():
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0: 0}, "")
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0: 0}, "a\tb")
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0: 0}, "x" * 33)
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0: 0}, "ok", label="ї" * 15)   # 30 bytes > 28
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0: 0}, "ok", flags=package.CREATED_BY_COPY)
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {0x800: 0}, "ok", code_words=0x800)
    with pytest.raises(package.PackageError):
        package.build(0xAA1, "PFS154", {}, "ok")


def test_default_name():
    assert package.default_name("/x/y/helloworld_pfs154.ihx") == "helloworld_pfs154"
    assert package.default_name("C:\\fw\\a.b.hex") == "a.b"
    long = "я" * 20   # 40 bytes
    assert len(package.default_name(long + ".ihx").encode()) <= 32


def test_ihex_records_and_checksum():
    text = ":020000040000FA\n:0400000070000000 8C\n".replace(" ", "")
    mem = ihex.parse(text)
    assert mem == {0: 0x70, 1: 0, 2: 0, 3: 0}
    assert ihex.to_words(mem) == {0: 0x0070, 1: 0x0000}
    with pytest.raises(ihex.IhexError):
        ihex.parse(":0400000070000000 8D".replace(" ", ""))
    with pytest.raises(ihex.IhexError):
        ihex.parse("0400000070000000 8C".replace(" ", ""))
    # extended segment address (02)
    seg = ":020000021000EC\n:01000000AA55\n:00000001FF\n"
    assert ihex.parse(seg) == {0x10000: 0xAA}


def test_half_word_reads_ff():
    assert ihex.to_words({5: 0x12}) == {2: 0x12FF}


def test_format_round_trip():
    words = {0: 0x0070, 1: 0x3FFF, 7: 0x1234, 0x9000: 0xABCD}
    assert ihex.to_words(ihex.parse(ihex.format_words(words))) == words
