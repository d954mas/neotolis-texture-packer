# Vendored: utf8proc

Unicode normalization (NFC) and case-folding for persistent source-key
canonicalization (master spec §5.3; decision `docs/decisions/0002-utf8proc-nfc.md`).

## Pinned version

| Field | Value |
|-------|-------|
| Library | utf8proc |
| Version | **2.9.0** (`UTF8PROC_VERSION_MAJOR.MINOR.PATCH` = 2.9.0) |
| Upstream | https://github.com/JuliaStrings/utf8proc |
| Unicode data | Unicode 15.1.0 (shipped in the 2.9.0 release) |
| Licence | MIT "expat" (see `LICENSE.md`) |
| Source artifact | `utf8proc-2.9.0.tar.gz` (maintainer release asset) |
| Download URL | https://github.com/JuliaStrings/utf8proc/releases/download/v2.9.0/utf8proc-2.9.0.tar.gz |
| Release tarball SHA-256 | `bd215d04313b5bc42c1abedbcb0a6574667e31acee1085543a232204e36384c4` |

The maintainer-uploaded release **asset** is pinned (byte-stable), not the
GitHub auto-generated `archive/refs/tags` tarball (whose bytes are not
guaranteed stable over time).

## Files included

Only the amalgamation needed to link the library is vendored:

| File | SHA-256 | Purpose |
|------|---------|---------|
| `utf8proc.h` | `29a3a51647270444a84b10a5fe182f3469ccd205907998baf8370a4cbbdb2103` | Public API |
| `utf8proc.c` | `fe5255fb4f05b1d9f063ef104f3e0935097f298564b9b77d904e19b14a75d3bb` | Implementation (`#include`s `utf8proc_data.c`) |
| `utf8proc_data.c` | `f5538036d1cf17b9ef81647c9c7277c52f0106964582a8102ab9a4901c23c039` | Generated Unicode property/composition tables |
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
