# Architecture foundation M0 baseline

Date: 2026-07-15

Branch: `impl/master-spec`

Pre-M0 commit: `46b172b73c2238d863373684ceb919a1fd383300`

## Scope and method

This is the M0 measurement-integrity baseline. It records current production
paths; timing thresholds remain advisory. Every timed sample is accepted only
after its status, revision/state transition, output, and scenario-specific
postconditions pass. Failed operations are counted separately and invalidate
the run. Setup, fixture cloning, journal preparation, and cleanup are outside
the timed regions.

Reference host/toolchain: Windows x86-64 on an Intel Core i9-14900HX, active
Windows power scheme `Balanced`, CMake 3.29.6, Ninja 1.12.1, Clang 19.1.7
(`x86_64-pc-windows-msvc`). This was a foreground desktop run without CPU
affinity or service isolation. Release configuration, two warm-up runs and 20
recorded samples were used. The clock is `nt_time_now`, reported by the harness
as monotonic.

Commands:

```powershell
cmake --preset native-release
cmake --build --preset native-release --target tp_bench_foundation tp_bench_gui_rows tp_test_bench_support
build\_cmake\native-release\packer\tests\tp_bench_foundation.exe build\_cmake\native-release\tp_bench_foundation_run 20 100
build\apps\gui\native-release\tp_bench_gui_rows.exe build\_cmake\native-release\tp_bench_gui_rows_run 20
```

`tp_test_bench_support` pins nearest-rank p50/p95 calculation and verifies that
a failed or non-finite sample is excluded and makes a run invalid.

## Core fixture shape

| Fixture | Atlases | Sources | Overrides | Animations | Frames | Serialized bytes |
|---|---:|---:|---:|---:|---:|---:|
| NORMAL | 3 | 12 | 60 | 9 | 72 | 17,141 |
| LARGE | 20 | 80 | 4,000 | 100 | 1,600 | 787,035 |
| HUGE | 100 | 200 | 100,000 | 0 | 0 | 17,356,035 |

All fixtures serialize and load before measurement. Each recovery fixture has
100 accepted transaction records.

## Core results

Values are milliseconds. Every row has `accepted=20`, `failed=0`. Undo/Redo
uses the production file journal backend, so its timed append includes the
durable file write/flush rather than a memory-IO substitute.

| Scenario | NORMAL p50 / p95 | LARGE p50 / p95 | HUGE p50 / p95 |
|---|---:|---:|---:|
| Normal single-field transaction | 0.0099 / 0.0144 | 0.2974 / 0.5701 | 12.2058 / 15.9232 |
| Journal-backed Undo | 0.4361 / 0.8317 | 26.4382 / 27.9587 | 1,345.0064 / 1,650.2137 |
| Journal-backed Redo | 0.4088 / 0.6574 | 26.8720 / 41.3725 | 1,304.4233 / 1,512.7460 |
| Recovery, 100 records | 1.0278 / 1.4312 | 42.1861 / 47.0289 | 1,304.5952 / 1,617.1146 |
| Save + mark-saved + compact | 1.5699 / 1.9448 | 46.5894 / 54.0994 | 1,313.5874 / 1,457.4436 |

Measured count evidence:

- maximum clone allocations for a normal transaction: NORMAL 245, LARGE
  10,022, HUGE 200,802;
- maximum Undo append bytes: NORMAL 17,198, LARGE 787,092, HUGE 17,356,092;
- maximum Redo append bytes: NORMAL 17,218, LARGE 787,112, HUGE 17,356,112;
- 100-record recovery journal bytes: NORMAL 53,684, LARGE 823,578, HUGE
  17,392,578.

The canonical HUGE result triggers the compact-history acknowledgement: file-
backed Undo and Redo are roughly 30-33 times over the initial 50 ms p95 review
target and append a full-project-scale checkpoint. The normal transaction p95
meets its initial 16.7 ms advisory target, while 100-record HUGE recovery misses
the initial 1 s target. These are routed findings, not M0 timing hard-failures.

## GUI row results

| Fixture | Rows | Overrides | Scan bytes | p50 / p95 ms | Steady row reallocs | FS calls per rebuild | Directory walks |
|---|---:|---:|---:|---:|---:|---:|---:|
| NORMAL | 256 | 64 | 199,920 | 0.1008 / 0.1311 | 0 | 2 | 0 |
| LARGE | 4,096 | 1,024 | 3,210,480 | 7.1310 / 9.0130 | 0 | 2 | 0 |
| HUGE | 10,000 | 9,999 | 7,839,216 | 106.2009 / 113.0795 | 0 | 2 | 0 |

Every row has `accepted=20`, `failed=0`. Synthetic cached scan results remove
directory walking from the timed region while the production `build_rows`,
`gui_rows`, and `gui_scan` code remains in the executable.

## Routed findings

These are measured or directly counted current-path deficits. M0 reports them;
their owning packages remove them.

- M2: a steady row rebuild performs one existence and one directory metadata
  probe per source. The target unchanged-frame contract is zero filesystem
  calls.
- M2: sprite override lookup is performed for each displayed child. The
  10,000-row/9,999-override result is consistent with the current
  `O(rows * overrides)` implementation and misses the initial 8 ms review
  target. M2 owns the indexed `O(rows + overrides)` path.
- M3: journal recovery currently materializes every operation with one
  `malloc` plus one payload `memcpy` in `tp_journal_scan`. The target maximum is
  zero payload copies, and M3 owns bounded parsing/materialization. The canonical
  HUGE 100-record recovery p95 of 1.617 s also misses the initial 1 s review
  target.
- P-UNDO: canonical HUGE Undo/Redo durably appends about 17.36 MB and misses the
  initial 50 ms review target by more than an order of magnitude. The compact
  history packet is therefore triggered after M0.

No M1-M5 implementation is included in this baseline. In particular, no actor
thread, COW model, arena-backed mutable model, generic manager, or alternate
mutable source of truth was introduced.
