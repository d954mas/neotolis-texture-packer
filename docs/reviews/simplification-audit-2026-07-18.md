# Simplification audit — 2026-07-18

## Scope and baseline

Аудит выполнен после canonical foundation commit `c672ff4`, до добавления новых
Import/Export IR, registry, MCP и Dev API функций. Нормативный источник —
`docs/ntpacker-master-spec.md`; engine submodule не изменялся. Исходный baseline:
clean tracked tree, отдельный пользовательский untracked `.serena/`, build green,
84/84 tests green.

Цель прохода — убрать доказанное дублирование и зафиксировать ратчет сложности,
не меняя product contracts. Новый feature work в этот проход не входит.

## Responsibility map

| Module | Owned responsibilities | Audit decision |
|---|---|---|
| `tp_fs_internal.c` | strict UTF-8 path admission; Win32/POSIX file and directory backend; checked I/O, sync and publication primitives | Один правильный owner, но 3 подответственности в одном TU. Сначала устранён frontend policy duplicate; platform/TU split остаётся mechanical follow-up. |
| `tp_history_codec.c` | wire primitives; project/diff record codec; decoded-record validation; atomic replay adapter | Codec и replay orchestration смешаны. Разделять только отдельным contract-preserving packet с byte-for-byte v1 goldens. |
| `tp_project_identity.c` | pack-domain checks; structural ID assignment/uniqueness; canonical source/reference/graph gate | Pack-domain не относится к identity. Будущий split: constraints owner + ID owner + canonical graph validator. |
| `tp_json_internal.c` | raw JSON admission and duplicate-key rejection | Связная ответственность; оставить. |
| `tp_image.c` | bounded file ingress, RGBA8 decode, allocator-matched release | Связная ответственность; оставить. |
| `tp_utf8.c` | strict scalar width and byte/C-string validation | Связная ответственность; оставить. |
| `apps/common/nt_utf8_argv.c` | Windows process/environment UTF-16 ingress and argv ownership | Правильный client-ingress owner. Не переносить CLI parsing в core. |
| `apps/common/nt_utf8_fs.c` | CRT-local `fopen/remove/rename` adapters | После F-01 не владеет decoding, long-path, namespace или Win32 error policy. |

Отдельный legacy hotspot — `tp_project.c` (3387 LOC): path algorithms, mutable
CRUD, deterministic writer/save publication, JSON schema/parser и pack bridge.
Его физическое разделение необходимо, но не должно совмещаться с изменением Save
semantics или canonical-v5 schema.

## Findings and classification

| ID | Severity | Classification | Finding | Result |
|---|---|---|---|---|
| F-01 | high | auto-fixable after CRT review | `apps/common/nt_utf8_fs.c` repeated core UTF-8→UTF-16, long-path and namespace policy; implementations had already drifted at the 32767-character bound | Fixed in `c508eea`. Core exports only caller-buffer path conversion; CRT-local `FILE*` ownership stays in each client adapter. Frontend implementation fell from 256 to 86 LOC. R21 prevents policy duplication from returning. |
| F-02 | medium | auto-fixable | Canonical source-key normalize+compare existed in identity, operation admission and validation report code | Fixed in `f1de038` through `tp_srckey_validate_canonical()`. Each caller still maps status/message/severity independently. |
| F-03 | medium | auto-fixable | No enforceable source-size budget; large files could grow invisibly | Fixed by `architecture_loc_budget` and the exact legacy ratchet in `scripts/check_loc_budget.cmake`. |
| F-04 | medium | auto-fixable, not attempted | Slash-normalized source-path text admission/equality is repeated in CRUD, canonical identity and operation preflight | Extract only bounded text admission/hash/equality. Do not merge effective canonical path identity or portability/case-fold reporting. |
| F-05 | high | manual-only | Atlas settings and sprite override constraints are expressed in operation admission, validation reporting and defensive Pack checks | Introduce a pure typed constraints evaluator later. Preserve three policies: first reject, accumulated findings, and final engine-assertion guard. Requires a cross-layer boundary-vector matrix. |
| F-06 | medium | manual-only | `tp_project`, `tp_validate`, `tp_op_validate`, history codec and filesystem TUs exceed the responsibility/complexity budget | Split by the map below, one oracle-backed packet at a time. Parser/save and history replay are highest risk. |

## Validation consolidation boundary

Safe to share:

- pure canonical-key predicates (F-02);
- bounded source-path text admission/equality (F-04);
- pure pack constraint evaluation returning typed issues (F-05);
- exact capability and exporter-ID predicates already shared by core helpers.

Must remain separate:

- operation admission returns the first structured reject and field;
- `tp_validate` accumulates stable ordered findings and severities;
- `tp_pack` defensively blocks values that could reach assertion-based engine code;
- canonical project admission is a hard persistence gate, not a user-facing
  warning policy;
- history decode/replay validates untrusted bytes before swapping model state.

Combining the pure predicate is simplification. Combining these policies would
weaken contracts or destabilize structured output.

## LOC and complexity budget

Production C excludes vendor, generated, test, benchmark, pack-builder and GUI
selftest sources.

| Measure | Soft target | Hard limit |
|---|---:|---:|
| Translation unit | 500 physical LOC | 800 physical LOC |
| Function | 60 physical LOC | 120 physical LOC |
| Cognitive complexity | 15 | 30 |
| Nesting depth | 3 | 4 |
| Parameters | 4 | 6 |

New code may not exceed hard limits. Existing TU violations have exact no-growth
limits in `scripts/check_loc_budget.cmake`; every shrink must lower the baseline.
Existing function violations are not an allow-to-grow list. A touched function
must not increase LOC/CC and should move toward a family handler below the hard
limit. Dispatcher switches may exceed CC only when cases delegate and contain no
business rules.

Clang-Tidy `readability-function-cognitive-complexity` at threshold 25 measured
the current main hotspots:

| Function | CC |
|---|---:|
| `tp_operation_validate` | 154 |
| `validate_operation_utf8` | 114 |
| `validate_atlas` | 92 |
| `tp_project_reject_unknown_schema_keys` | 85 |
| `tp_project_json_admit` | 74 |
| `tp_project_save_stage` | 71 |
| `tp_pack::validate_settings` | 70 |
| `validate_sources` | 62 |
| `encode_op` | 52 |

Recommended physical cuts, in risk order:

1. split operation validation by operation family;
2. split validation indexes/materialization from atlas rules;
3. split filesystem platform backends from checked durability helpers;
4. split project path/model/writer/parser/bridge TUs without semantic edits;
5. split history writer, reader/schema and replay adapter under byte goldens.

## Verification

- baseline: `cmake --build --preset native-debug`; 84/84 CTest passed;
- F-01: Windows Unicode/long-path/raw-namespace tests preserved; full build and
  84/84 CTest passed;
- F-02: new canonical-key unit test failed before the API existed, then targeted
  source-key/operation/schema/validation tests and full 84/84 CTest passed;
- F-03: `cmake -P scripts/check_loc_budget.cmake` passes over 87 production TUs;
  the gate is registered as a normal cross-platform CTest.

## Next simplification packets

Continue without feature work until each packet is independently green:

1. F-04 source-path text identity primitive;
2. operation-family handler split, using exact operation tests as oracle;
3. validation-family split, preserving ordered JSON findings;
4. filesystem TU split;
5. only then decide the F-05 pack-constraints evaluator design.

