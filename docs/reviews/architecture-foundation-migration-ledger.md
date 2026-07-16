# Architecture foundation migration ledger

Этот ledger фиксирует закрытие старых authoritative paths для M1–M5. Он не
создаёт новых продуктовых решений: нормативным источником остаётся
`docs/ntpacker-master-spec.md`.

| Family | Единственный owner | Тонкий adapter | Удалённый старый path | Evidence |
|---|---|---|---|---|
| Atlas lifecycle, rename, settings | typed operations + `tp_session` admission | GUI intent capture; CLI one-shot transaction | frontend project mutation and direct field writes | `tp_transaction`, `tp_gui_session_adapter`, `cli_mutate_atlas`, `scripts/check_boundaries.sh` R6/R7/R8 |
| Sources, including batch add/remove | typed operations; shared source plan/runtime generation | GUI captures stable atlas/source IDs; CLI resolves selectors | GUI/CLI source mutation and duplicated scan/naming policy | `tp_input`, `tp_session`, `tp_client_parity`, `cli_mutate_source` |
| Sprite rename and overrides | canonical `(source_id, source_key)` records in project/operation core | GUI captures `gui_sprite_ref`; CLI resolves one structural selector | name-only authoritative override lookup and the `tp_input` pending-name fallback | `tp_operation`, `tp_transaction`, `tp_input` duplicate-key regression, `tp_client_parity` |
| Animation lifecycle/settings/frames | canonical `{source_id, src_key}` frame refs in typed ops, history/journal, saved storage, and export mapping | GUI/CLI resolve human sprite selectors before admission and only marshal canonical refs | name-only frame payloads in new ops and export-key-only frame matching | `tp_operation` canonical wire golden, `tp_diff`, `tp_transaction`, `tp_export_run` duplicate-source-key export regression, `tp_gui_session_adapter`, `cli_mutate_anim`, `cli_mutate_sprite` |
| Legacy sprite/frame migration | project-level clone-and-swap at writable open/save and immutable snapshot load | clients receive structured ambiguous-reference failure | atlas-by-atlas partial re-key and applying unresolved pending records by name | `tp_sprite_migrate` ambiguity/no-mutation + orphan/reactivation tests, `tp_session` writable open/save/orphan/reactivation regression, `cli_parity` v1 fixture rewrite/read path |
| Export targets | typed operations + registry/validation core | GUI/CLI pass exporter ID/path/enabled intent | frontend target mutation and capability validation | `tp_client_parity`, `cli_mutate_target`, boundary R2/R6/R7 |
| Save, Save As, Undo, Redo, Discard | `tp_session`; model owns dirty/history semantics | clients map typed status/receipt | frontend dirty/Undo policy and mutable project save authority | `tp_session`, `tp_project_lease`, boundary R8/R10 |
| Snapshot reads and GUI rows | one owned immutable `tp_session_snapshot`; rows cache by model/source generations | views read DTOs only | mutable `tp_project *` escape and per-frame model/FS rebuild | `tp_session`, `tp_bench_gui_rows`, boundary R8/R11/R12 |
| Pack, preview, Export jobs | session-owned typed job handle; algorithms remain in `tp_build` | GUI starts/polls/takes typed results | GUI worker authority and direct blocking `tp_pack`/export orchestration | `tp_session`, `tp_client_parity`, `ntpacker_gui_selftest`, boundary R10/R13 |
| Recovery and project authority | root-scoped `tp_recovery` flow + live/claim + `tp_project_lease` | GUI supplies app-data root and maps candidate/result DTOs | GUI/domain live-slot lifecycle, naming, exclusion, scan/claim state machine | `tp_recovery_store`, `tp_project_lease`, boundary R9/R10 |
| Headless parity | shared transaction/error corpus and in-process session adapter | CLI remains saved-file one-shot; live harness exposes supported capabilities | client-specific business-rule forks and false surface parity | `tp_client_parity`, CLI mutation families, capability tests |

Для каждой строки gate считается закрытым только вместе с executable boundary
check и указанными parity/fault tests; наличие adapter само по себе не является
доказательством cutover.
