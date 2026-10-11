# SPDX-License-Identifier: GPL-3.0-or-later
"""Intel HEX: reading (records 00/01/02/04) and writing 8-bit HEX of a word image."""
from __future__ import annotations


class IhexError(ValueError):
    pass


def parse(text: str) -> dict[int, int]:
    """Returns {byte_address: value}. Checks every line checksum."""
    mem: dict[int, int] = {}
    base = 0
    for no, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise IhexError(f"line {no}: missing ':'")
        try:
            b = bytes.fromhex(line[1:])
        except ValueError:
            raise IhexError(f"line {no}: not hex") from None
        if len(b) < 5 or len(b) != b[0] + 5:
            raise IhexError(f"line {no}: bad length")
        if sum(b) & 0xFF:
            raise IhexError(f"line {no}: bad checksum")
        n, addr, typ, data = b[0], b[1] << 8 | b[2], b[3], b[4:4 + b[0]]
        if typ == 0x00:
            for i, v in enumerate(data):
                mem[base + addr + i] = v
        elif typ == 0x01:
            break
        elif typ == 0x02:
            base = (data[0] << 8 | data[1]) << 4
        elif typ == 0x04:
            base = (data[0] << 8 | data[1]) << 16
        elif typ in (0x03, 0x05):
            pass   # start address: irrelevant for PADAUK
        else:
            raise IhexError(f"line {no}: unknown record type {typ:02X}")
    return mem


def to_words(mem: dict[int, int]) -> dict[int, int]:
    """Byte image → {word_address: value} (little endian); a missing half of a word reads as 0xFF."""
    return {a // 2: mem.get(a & ~1, 0xFF) | mem.get(a | 1, 0xFF) << 8 for a in mem}


def read_words(path: str) -> dict[int, int]:
    with open(path, encoding="ascii", errors="strict") as f:
        return to_words(parse(f.read()))


def _record(typ: int, addr: int, data: bytes) -> str:
    b = bytes([len(data), addr >> 8 & 0xFF, addr & 0xFF, typ]) + data
    return ":" + (b + bytes([-sum(b) & 0xFF])).hex().upper()


def format_words(words: list[int] | dict[int, int], per_line: int = 16) -> str:
    """Words → 8-bit Intel HEX text (byte addresses, low byte first), the format easypdkprog writes."""
    items = sorted(words.items()) if isinstance(words, dict) else list(enumerate(words))
    mem = {}
    for a, w in items:
        mem[2 * a] = w & 0xFF
        mem[2 * a + 1] = w >> 8 & 0xFF
    lines, upper = [], 0
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        if start >> 16 != upper:
            upper = start >> 16
            lines.append(_record(0x04, 0, bytes([upper >> 8, upper & 0xFF])))
        chunk = bytearray()
        while (i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < per_line
               and addrs[i] >> 16 == upper):
            chunk.append(mem[addrs[i]])
            i += 1
        lines.append(_record(0x00, start & 0xFFFF, bytes(chunk)))
    lines.append(_record(0x01, 0, b""))
    return "\n".join(lines) + "\n"
