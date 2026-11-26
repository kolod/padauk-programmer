# Contract: firmware package format (format version 1)

A package is the unit that the client builds from a firmware file and sends to the programmer (`PKG_BEGIN` /
`PKG_DATA` / `PKG_COMMIT`), and that the programmer stores unchanged in a flash slot. All fields are little-endian.
CRC is CRC-32/ISO-HDLC (`zlib.crc32`).

```
offset          size          field
0               128           Header
128             2·N           Words[N]           u16, word i = program memory address i
128+2N          2·⌈N/16⌉      DefinedMask        bit i (LSB-first within a byte) = 1 ⇔ Words[i] is defined
128+2N+2⌈N/16⌉  4             package_crc32      CRC of all preceding bytes of the package
```

`package_size = 128 + 2·N + 2·⌈N/16⌉ + 4`.

## Header (128 bytes)

| Off | Type | Field | Rules |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | ASCII `"PDKP"` |
| 4 | `u16` | `format_version` | `1` |
| 6 | `u16` | `header_size` | `128` |
| 8 | `u32` | `package_size` | per the formula above; ≤ `max_package_size` from `GET_INFO` |
| 12 | `u16` | `chip_id12` | the 12-bit IC ID from `LIST_CHIPS` |
| 14 | `u16` | `image_words` (N) | 1 … `code_words` of the IC |
| 16 | `u32` | `flags` | see below; unknown bits MUST be 0 |
| 20 | `u16` | `fuse` | the fuse value, if `FUSE_PRESENT` |
| 22 | `u16` | `run_vdd_mV` | voltage for `RUN`; 0 → the default 5000 |
| 24 | `u8[16]` | `reserved` | MUST be 0 (the serial number is always random, there are no parameters) |
| 40 | `u32` | `source_crc32` | CRC-32 of the source file (for display by the client) |
| 44 | `u32` | `created_unix` | the time the client created the package; 0 — unknown |
| 48 | `char[16]` | `chip_name` | a name or name variant from `LIST_CHIPS`, NUL-padded; must correspond to `chip_id12` |
| 64 | `char[32]` | `name` | **package name**: UTF-8, NUL-padded, non-empty, no control characters (`< 0x20`); for lists and the screen; the client by default uses the source file name without the extension |
| 96 | `char[28]` | `label` | optional description: UTF-8, NUL-padded |
| 124 | `u32` | `header_crc32` | CRC-32 of bytes 0…123 |

### `flags`

| Bit | Name | Meaning |
|---|---|---|
| 0 | `FUSE_PRESENT` | write `fuse` |
| 1 | `ALLOW_SECURITY_FUSE` | allow a fuse that enables read protection |
| 2 | `SKIP_ERASE` | do not erase (FLASH) before writing |
| 3 | `SKIP_BLANK_CHECK` | do not check for blank — **FLASH only**; for an OTP IC → `PKG_OTP_BLANK_CHECK_REQUIRED` |
| 4 | `SKIP_VERIFY` | do not verify |
| 5 | — | reserved, MUST be 0 (every stub is calibrated; to not calibrate, do not place the macro) |
| 6 | `SECURE_FILL` | fill undefined words with NOP up to the reserved areas |
| 7 | — | reserved, MUST be 0 (the serial-number stub is always filled when present) |
| 8 | `CREATED_BY_COPY` | the package was created by the `COPY` operation; the client MUST NOT set it |
| 9–31 | — | reserved, MUST be 0 |

The placeholder of a calibration stub is never written to the IC as is: in its place a measurement loop is always
written, which is finished after calibration (for OTP — one full write and
one rewrite of the stub).

## Programmer checks

**In `PKG_BEGIN` (the header only):**

| Check | Error |
|---|---|
| `magic`, `format_version`, `header_size`, `header_crc32`, reserved bits | `PKG_HEADER_INVALID` |
| `name` empty, invalid UTF-8 or control characters; invalid UTF-8 in `label` | `PKG_HEADER_INVALID` |
| `package_size` matches N and is ≤ the slot capacity | `PKG_TOO_LARGE` / `PKG_HEADER_INVALID` |
| `chip_id12` + `chip_name` are in the table | `PKG_CHIP_UNSUPPORTED` |
| the IC supports writing | `PKG_CHIP_READ_ONLY` |
| `N` ≤ `code_words` | `PKG_TOO_LARGE` |
| a protecting fuse without `ALLOW_SECURITY_FUSE` | `PKG_SECURITY_NOT_ALLOWED` |
| `SKIP_BLANK_CHECK` for an OTP IC (constitution, principle III) | `PKG_OTP_BLANK_CHECK_REQUIRED` |

**In `PKG_COMMIT` (the whole package, read back from flash):**

| Check | Error (+ `addr`) |
|---|---|
| `package_crc32` matches | `PKG_CRC_MISMATCH` |
| defined words do not fall into the IC's reserved areas | `PKG_RESERVED_AREA` |
| the defined last word (fuse) does not contradict `FUSE_PRESENT`/`fuse` | `PKG_FUSE_CONFLICT` |
| a protecting fuse from the image without `ALLOW_SECURITY_FUSE` | `PKG_SECURITY_NOT_ALLOWED` |
| all calibration stubs are recognized | `PKG_CALIB_UNKNOWN` |
| at most 16 calibration stubs (`addr` — the 17th) | `BAD_PAYLOAD` |

Word bits above the IC core width are ignored.

## Effective image (what is actually written)

The programmer computes it on the fly from the package:

1. defined words; undefined ones are "do not write" (all ones), or NOP if `SECURE_FILL`;
2. the last memory word is excluded from the main write and is written as the fuse in the last step;
3. calibration stubs are replaced by a measurement loop, and after calibration by `MOV A,#k` + NOP;
4. serial-number stubs receive the bytes of a new random 64-bit number (least significant first).

## Packages created by `COPY`

The programmer itself builds a package from a reference dump (`chip_name`; `name` — from the `ext` of `COPY` or `"copy <chip_name> <crc>"`,
where `<crc>` is the 8 hex digits of the reference's `image_crc_norm`; `label` empty; the fuse from the
last word; the reference's serial number, if it has one, is **not recognized** and is carried over as is — FR-052). Calibration traces (research R9) are replaced by the standard
free-pdk stubs:

- IHRC/ILRC: `'R','C', kind, freq[0..3], mV[0..1], reg` with the determined target;
- BandGap: `'B','G', 3, 3950 & 0xFF, 3950 >> 8, bgtr, gpcc, gpcs, 0 × 6` with the addresses from the IC table.

**The internal frequency value `0xFF0000kk`** in an IHRC/ILRC stub means "target unknown: at `PROGRAM`
measure the copy with register value `kk`, round to a standard SYSCLK (`SNAP_STANDARD`); if the
deviation is > 10 % — write `kk` without calibration". Such a value is impossible in a stub from a real
program (> 4 GHz), so the programmer accepts it only in packages with the `CREATED_BY_COPY` flag
(bit 8), which only the programmer itself sets; a package from a PC with this bit → `PKG_HEADER_INVALID`.

## Reference values

Example: PFS154 (`chip_id12 = 0x0AA1`), an image of 3 words, no fuse:
`N = 3`, `DefinedMask` = 2 bytes `07 00`, `package_size = 128 + 6 + 2 + 4 = 140`.
The package test vectors are stored in `software/tests/vectors/` and are used by both the firmware and the client.
