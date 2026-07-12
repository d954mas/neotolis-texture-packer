# C0-04 — Deterministic raster color/orientation contract note

Status: spike-accepted. Normative source: `docs/ntpacker-master-spec.md`
§10.2 (pack-input / semantic image hash), §11.1 (canonical raster input),
§59 item 28 (raster normalizes to deterministic RGBA8), §60 item 7 (the OPEN
color-management/orientation decision this spike closes), and the already-decided
`docs/decisions/0005-raster-color-policy.md`. This note pins the cross-platform
rules for decoding raster sources into canonical RGBA8 **before** the semantic
image hash becomes a contract (B1-01 task 8, F3-03 task 1). It ships **no
production decoder** — B0-03/B1-01 build that and MUST implement this policy —
only a decoder-independent reference normalization pipeline plus golden RGBA8
vectors that are its executable form. It reuses the `tp_c0` library (`tp_c0_detail`
structured errors, never abort) from C0-01..03.

Canonical target (§11.1), taken over by `semantic_image_hash = hash(width +
height + canonical RGBA8 pixels)` (§10.2):

    RGBA8 · rows top-to-bottom · channels R,G,B,A · straight alpha ·
    no row padding · orientation already applied

Reference implementation and golden tests:

- `include/tp_c0/tp_c0_raster.h` + `src/tp_c0_raster.c` — the format-independent
  normalization pipeline: channel expansion + A=255, the pinned 16→8 reduction,
  the 8 EXIF-orientation pixel transforms, the ICC-notice policy, container
  probing, and EXIF-orientation-tag parsing (stb never does this). Pure
  fixed-width integer math — no libm, no float, no host-endianness — so the
  canonical bytes are byte-identical on Linux/macOS/Windows.
- `tests/test_c0_raster.c` (`test_c0_raster` ctest) — synthetic vectors: the
  authoritative all-8-orientations proof on an asymmetric non-square pattern,
  the reduction pins, expansion, ICC/EXIF-unknown policy, EXIF parse.
- `include/tp_c0/tp_c0_stb.h` + `src/tp_c0_stb_impl.c` — the isolated stb_image
  glue (the ONLY stb TU in the spike; see §7).
- `tests/test_c0_raster_decode.c` (`test_c0_raster_decode` ctest) — REAL
  end-to-end vectors: PNG palette / grayscale-8 / grayscale-16 / RGB-no-alpha
  (built in-repo by `tests/tp_c0_png_write.h`, a minimal deterministic PNG
  encoder) and a fixed JPEG blob (`tests/tp_c0_jpeg_fixture.h`) decoded no-alpha
  and EXIF-orientation-tagged.

Structured tokens extend the C0-01/02/03 `tp_c0_detail` vocabulary (append-only,
pinned in `test_c0_error`, `TP_C0_DETAIL_COUNT` sentinel kept last). New:
`decode_failed`, `format_unsupported`, `icc_ignored`, `icc_profile_bad`,
`exif_orientation_unknown`. The notice tokens ride an out `tp_c0_raster_notices`
list — a notice is never a function's error return on a successful decode.

---

## 1. Baseline inventory (task 1) — current decode vs. target policy

The packer has **no** canonical decoder yet. Today's decode paths and how each
differs from the target policy:

- **Engine builder (path sources).** `nt_builder_atlas_add`
  (`external/neotolis-engine/tools/builder/nt_builder_atlas.c:728`) decodes via
  `stbi_load_from_memory(..., req_comp=4)` → RGBA8. Divergences from policy that
  the packer-side decoder (B0-03/B1-01) must correct:
  - stb **does not apply EXIF orientation** (it ignores EXIF entirely). Policy
    requires orientation applied before analysis/packing.
  - stb reduces 16-bit → 8-bit by `(v >> 8)` **truncation**
    (`stb_image.h:1200`). Policy requires round-to-nearest (§3).
  - stb ignores ICC (matches policy: not applied) but surfaces **no notice**.
    Policy requires a structured notice for a present/corrupt profile (§5).
  - stb has **no WebP** decoder at all (§6).
  - stb's grayscale/gray+alpha/palette → RGBA8 expansion and A=255 for no-alpha
    already match policy.
- **GUI thumbnails.** `apps/gui/gui_canvas.c:12` decodes via stb for preview
  only; thumbnails are a lazy CPU/GPU LRU, ≤256 px (§59 item 26), never a Pack
  semantic input — out of scope for the hash.
- **Packer `tp_export_png.c`.** Write-only (PNG page output); performs no decode.

## 2. Reference normalization pipeline (task 2) — `tp_c0_raster`

Format-independent steps applied to already-decoded samples (decision 0005):

- **Channel expansion + straight-alpha A=255** — `tp_c0_raster_expand8` /
  `tp_c0_raster_expand16`: gray→RGB replicate; gray+alpha→RGBA; RGB→RGBA with
  **A=255**; RGBA passthrough. Palette is expanded to direct color by the decoder
  before this layer (stb always returns direct color), so there is no palette
  layout here — the palette→RGBA golden (§4) proves the decoded result.
- **16-bit→8-bit** — pinned rule, §3.
- **EXIF orientation** — pure pixel transform, §3/§5. Output is
  "orientation already applied".
- **ICC** — never applied; pixels never change; a present profile yields a
  notice, §5.

## 3. PINNED DECISION — 16-bit → 8-bit reduction rule

`tp_c0_raster_reduce16(v)` = **round to nearest** over the 0..65535 domain:

    reduced = ((uint32_t)v * 255u + 32767u) / 65535u       // unsigned, no overflow

Ties are impossible (`v*255/65535` is never an exact half for integer `v`), so
this is exact round-to-nearest with no rounding-direction ambiguity, and it is
platform-independent (fixed-width unsigned integer math). It **differs from stb's
`stbi_load` `v>>8` truncation**: e.g. `0x00FF → 1` (truncation `0`), `0x01FF → 2`
(truncation `1`). **Consequence for B0-03/B1-01:** decode 16-bit sources via
`stbi_load_16` (or an equivalent that preserves 16-bit) and reduce through this
rule — using stb's default `stbi_load` would change the semantic image hash.
Proven by `test_reduce16_rounds_not_truncates` and, end-to-end on a real 16-bit
PNG, by `test_png_gray16_uses_rounding_not_truncation` (which also captures the
stb-truncation baseline for contrast).

## 4. Golden RGBA8 decode vectors (task 3) — byte-identical on 3 OS

Real end-to-end (decode → pipeline → canonical), all byte-exact goldens:

| Vector | Source | Real-decode? | Pins |
|---|---|---|---|
| PNG grayscale-8 | in-repo PNG | stb | gray→RGBA, A=255 |
| PNG palette-8 | in-repo PNG (PLTE) | stb | palette expansion, A=255 |
| PNG RGB-8 | in-repo PNG | stb | no-alpha → A=255 |
| PNG grayscale-16 | in-repo PNG | stb (`load_16`) | round-not-truncate reduction |
| JPEG no-alpha | fixed blob | stb (scalar) | no-alpha → A=255 |
| JPEG + EXIF orient 6 | fixed blob + spliced APP1 | stb (scalar) | EXIF parse + transform |
| WebP | synthetic RGBA | — (deferred, §6) | pipeline decoder-independence |

**Orientation coverage.** All 8 EXIF values are pinned exhaustively on an
asymmetric **3×2** pattern (unique per-pixel codes; non-square so
dimension-swapping orientations 5..8 are visible) in `test_orientation_all_eight`
— a wrong flip/rotate/axis-swap changes the golden. Mapping output (x,y) → source
(a,b), source dims (w,h); orientations 5..8 output (h,w):

    1 identity      a=x,b=y            5 transpose     a=y,b=x
    2 mirror-h      a=w-1-x,b=y        6 rotate 90 CW  a=y,b=h-1-x
    3 rotate 180    a=w-1-x,b=h-1-y    7 transverse    a=w-1-y,b=h-1-x
    4 mirror-v      a=x,b=h-1-y        8 rotate 90 CCW a=w-1-y,b=x

The JPEG orientation vector additionally proves the container path: EXIF is
parsed out of a real JPEG APP1/Exif TIFF IFD (both byte orders) by
`tp_c0_raster_exif_orientation`, stb-decoded pixels are confirmed unchanged by
the EXIF tag, then the parsed orientation drives the transform.

## 5. Malformed / unsupported handling (task 4)

- **Corrupt ICC** — `tp_c0_raster_note_icc(present, valid, notices)`:
  present+valid → `icc_ignored`; present+invalid → `icc_profile_bad`; absent →
  none. Pixels are **never** touched. `test_icc_corrupt_profile_pixels_unchanged`
  decodes a PNG with a corrupt `iCCP` chunk and asserts byte-identical pixels vs.
  the profile-free PNG, plus the `icc_profile_bad` notice.
- **PINNED DECISION — unknown EXIF orientation.** A tag value outside 1..8 is
  treated as **identity (orientation 1)** and emits the
  `exif_orientation_unknown` notice; decode does **not** fail. Rationale: a stray
  EXIF value is common in the wild and must not abort a Pack; identity is the safe
  deterministic default and the notice surfaces it. `test_orientation_unknown_is_identity_plus_notice`.
- **Truncated / garbage file** — `decode_failed` structured error, never a crash
  (`test_truncated_and_garbage_decode_failed`). stb returns NULL; the wrapper maps
  it. No UB: the stb TU is un-sanitized (§7) and the pipeline's own EXIF/orient
  code is bounds-checked (`test_exif_orientation_truncated_no_crash`).

## 6. PINNED DECISION — WebP decoder deferred to B1-01

stb cannot decode WebP, and vendoring `libwebp` is a **supply-chain decision that
belongs in B1 (import foundation), not this policy spike**. Pinned here:

- A WebP-signature buffer classifies as `TP_C0_CONTAINER_WEBP` →
  `format_unsupported` (the gate B1's decoder registry replaces). 
- The normalization policy is **decoder-independent**: synthetic raw RGBA samples
  (as a future WebP decoder would hand over) pass the pipeline unchanged
  (`test_webp_policy_deferred`). When B1-01 selects/vendors the WebP codec, only
  the decode front-end is added — this canonical policy is unchanged.

## 7. Determinism measures (hard invariant)

- **stb built with `STBI_NO_SIMD`** in its own `tp_c0_stb` TU. stb's SSE2 (x86)
  and NEON (arm) IDCT kernels are not guaranteed bit-identical, and NEON is not
  auto-enabled — so without this a JPEG golden could differ between x86 and arm64
  CI. The forced scalar path is all-integer, so JPEG goldens are byte-identical
  across CI OS. `STBI_NO_HDR`/`STBI_NO_LINEAR` remove the only float output paths.
  **B1-01 must make the same guarantee** (NO_SIMD, or a verified bit-identical
  SIMD path) or JPEG semantic hashes will diverge across platforms.
- **The JPEG encoder is never on the path.** `stbi_write_jpg` uses a float DCT
  (non-deterministic); the JPEG fixture is a fixed byte blob generated once and
  only ever decoded.
- **stb is isolated** (its own target, `-w`, no ASan/UBSan) so its internal
  impl-defined ops never trip the spike's `-Werror` or CI's
  `-fno-sanitize-recover=all`; the pure `tp_c0_raster` pipeline keeps full
  warnings + sanitizers. The two stb impls (engine `stb_image` with SIMD, spike
  `tp_c0_stb` without) are never linked into one executable.

## 8. Non-goals / still open (§60 item 7)

Full color management — ICC transforms, gamma conversion, wide-gamut — is **out
of v1** (decision 0005). Any future deviation from this policy (applying ICC,
changing the reduction rule, a different orientation convention) is a NEW decision
that changes the semantic image hash and requires a versioned hash-algorithm tag
(F1-03/F3-03). The concrete production decoder, its format registry, and WebP
vendoring are B0-03/B1-01.
