# Vendored: utf8proc

Unicode normalization (NFC) and case-folding for persistent source-key
canonicalization (master spec §5.3; decision `docs/decisions/0002-utf8proc-nfc.md`).

## Pinned version

| Field | Value |
|-------|-------|
| Library | utf8proc |
| Version | **2.11.1** (`UTF8PROC_VERSION_MAJOR.MINOR.PATCH` = 2.11.1) |
| Upstream | https://github.com/JuliaStrings/utf8proc |
| Unicode data | Unicode 17.0.0 (shipped in the 2.11.1 release) |
| Licence | MIT "expat" (see `LICENSE.md`) |
| Source artifact | `utf8proc-2.11.1.tar.gz` (maintainer release asset) |
| Download URL | https://github.com/JuliaStrings/utf8proc/releases/download/v2.11.1/utf8proc-2.11.1.tar.gz |
| Release tarball SHA-256 | `0aa41260917df1ef4724f34f314babbd48ba18963e4d5a14a1752f14ee765010` |

The maintainer-uploaded release **asset** is pinned (byte-stable), not the
GitHub auto-generated `archive/refs/tags` tarball (whose bytes are not
guaranteed stable over time).

## Files included

Only the amalgamation needed to link the library is vendored:

| File | SHA-256 | Purpose |
|------|---------|---------|
| `utf8proc.h` | `8b2ec05d2486d49f46d1a4e824f471efd3390bed7c38b999a5cf7de152003914` | Public API |
| `utf8proc.c` | `dfcfee948bb0a817b60ebf722fad00f0580f330a55a7b50793162ef4f0ef453b` | Implementation (`#include`s `utf8proc_data.c`) |
| `utf8proc_data.c` | `950e549dbfc853c4304425f3af1875e72fa9fc9697c273c763400c2da4e380a7` | Generated Unicode property/composition tables |
| `LICENSE.md` | — | Upstream licence text (MIT expat + Unicode data terms) |
| `CMakeLists.txt` | — | Our thin static-lib target (not upstream's build) |

Excluded from the vendor drop: upstream build files, tests, benchmarks, data
generators, docs, and CI config. They are not needed to link the library and
are recoverable from the pinned release if the data ever needs regenerating.

## Update procedure

1. Download the pinned release asset for the new version; record its SHA-256.
2. Replace `utf8proc.h`, `utf8proc.c`, `utf8proc_data.c`, `LICENSE.md`.
3. Update the version row and per-file SHA-256s above.
4. Re-run `ctest` (the `tp_c0_*` NFC/case-fold golden vectors are the regression
   gate for a data-table change).

Do not edit the vendored `.c`/`.h` by hand -- keep them byte-identical to
upstream so the checksums above stay verifiable.
