# Raster normalization goldens

Status: non-normative research fixture. The normative architecture remains
`docs/ntpacker-master-spec.md`; color policy is recorded in
`docs/decisions/0005-raster-color-policy.md`.

This artifact preserves the byte-exact parts of the retired pre-production
raster experiment that are not fully specified by the master spec. It is an
oracle for a future production decoder, not an API or an implementation
requirement by itself.

## Fixture encoding

Files under `docs/research/fixtures/raster/` are lowercase hexadecimal text.
Whitespace is insignificant; every two hex digits encode one byte in file
order.

| Fixture | Decoded size | SHA-256 of decoded bytes | Purpose |
|---|---:|---|---|
| `jpeg-quad8.hex` | 725 bytes | `ba458453345288dfdb86f8362ca44c352b52b6a3a0c49ec8324e2cec093bb685` | Fixed 8x8 baseline-DCT JPEG, 4:4:4, asymmetric red/green/blue/yellow quadrants, no EXIF |
| `exif-app1-le.hex` | 36 bytes | `231c6880b1638ff083ed323d18fb95c3b18b25379e00b512d8dca2aa9d7dd935` | APP1/Exif little-endian TIFF template; patch byte offset 28 with orientation 1..8 |
| `jpeg-quad8.rgba.hex` | 256 bytes | `db7d3c601dc09c48ee77fcefec2c06a2c23243153e9c0ed214dba2753d21134d` | Canonical 8x8 RGBA8 decode of the fixed JPEG |
| `jpeg-quad8-orient6.rgba.hex` | 256 bytes | `82c2faccf37b50edfd40e6d26ede4604c14f16edc219b84d5dd8d77d77edff2a` | Canonical RGBA8 after EXIF orientation 6 (90 degrees clockwise) |

To construct the orientation-6 JPEG, insert the 36 decoded APP1 bytes directly
after the JPEG SOI bytes and set byte 28 of the APP1 block to `06`. The JPEG
encoder is deliberately outside the oracle path.

The captured JPEG results assume a scalar integer decode path. A production
decoder may use SIMD only after proving byte-identical output on every supported
architecture; otherwise semantic image hashes can vary by platform.

## Canonical target

The normalized image is:

```text
RGBA8; rows top-to-bottom; channels R,G,B,A; straight alpha;
no row padding; orientation already applied
```

Channel expansion rules are gray -> `(g,g,g,255)`, gray-alpha ->
`(g,g,g,a)`, RGB -> `(r,g,b,255)`, and RGBA passthrough. Palette input is
expanded to direct color by the decoder before normalization.

The pinned unsigned 16-bit to 8-bit reduction is round-to-nearest:

```c
((uint32_t)v * 255u + 32767u) / 65535u
```

Useful discriminators against high-byte truncation are `0x00ff -> 1` and
`0x01ff -> 2`. Endpoints remain `0 -> 0` and `0xffff -> 255`.

## Orientation oracle

Source codes, row-major, are `10 20 30 / 40 50 60` for a 3x2 image. Each code
stands for an opaque gray RGBA pixel. Expected output dimensions and row-major
codes are:

| EXIF | Size | Expected codes |
|---:|---:|---|
| 1 | 3x2 | `10 20 30 / 40 50 60` |
| 2 | 3x2 | `30 20 10 / 60 50 40` |
| 3 | 3x2 | `60 50 40 / 30 20 10` |
| 4 | 3x2 | `40 50 60 / 10 20 30` |
| 5 | 2x3 | `10 40 / 20 50 / 30 60` |
| 6 | 2x3 | `40 10 / 50 20 / 60 30` |
| 7 | 2x3 | `60 30 / 50 20 / 40 10` |
| 8 | 2x3 | `30 60 / 20 50 / 10 40` |

Unknown orientation values use identity pixels and emit a structured notice;
they do not fail the decode. EXIF parsing must accept little- and big-endian
TIFF, skip repeated `ff` fill bytes before a marker, and reject truncated
segments without reading past the buffer.

## Reconstructable PNG vectors

These vectors are recipes rather than stored container bytes. A deterministic
PNG writer can reconstruct them without preserving an old test-only encoder.

| PNG input | Expected canonical RGBA8 |
|---|---|
| Gray8, 2x1: `50 200` | `50 50 50 255  200 200 200 255` |
| Palette8, 2x2: indices `0 1 2 3`; palette `(10,20,30) (40,50,60) (70,80,90) (100,110,120)`; no `tRNS` | `10 20 30 255  40 50 60 255  70 80 90 255  100 110 120 255` |
| RGB8, 2x1: `(10,20,30) (40,50,60)` | `10 20 30 255  40 50 60 255` |
| Gray16, 2x1: `0x0101 0x01ff` | `1 1 1 255  2 2 2 255` |

For the corrupt-profile vector, start with the RGB8 PNG above and insert an
`iCCP` chunk immediately after `IHDR`. Its seven-byte payload is
`6e 00 00 de ad be ef`: profile name `n`, NUL, compression method 0, then an
invalid deflate stream. Pixels must equal the profile-free decode and a
structured bad-profile notice must be emitted. A valid-but-ignored profile also
leaves pixels unchanged and emits an ignored-profile notice.

Truncated or garbage input must return a structured decode error, never abort.
The container probe signatures used by the research were PNG
`89 50 4e 47 0d 0a 1a 0a`, JPEG `ff d8 ff`, and WebP `RIFF....WEBP`.
