# Simplification plan review — 2026-07-18

**Verdict after revision:** ready for Phase 0–2; Phase 3–5 require the explicit
reassessment gate and bounded packet-plans.

## Review method

The repository has no configured `.planning` phase, so the automated GSD phase
convergence workflow could not be used as-is. A bounded manual convergence cycle
was used instead:

1. draft from the code review and responsibility map;
2. independent goal-backward review;
3. independent C/CMake sequencing and ownership review;
4. independent contract-oracle/adversarial review;
5. revision of the umbrella roadmap;
6. second-round review of the revised roadmap by the same three critics;
7. consistency and diff verification.

The three critics were read-only and reviewed the same draft independently.

## Accepted critique and plan changes

| Review finding | Disposition in revised plan |
|---|---|
| The 30+ packet document was too broad to execute directly | Recast as an umbrella roadmap. Execution is authorized only through Phase 2; every later split needs a bounded packet-plan and a reassessment gate. |
| History codec endian was stated incorrectly | Corrected to literal **big-endian** fixtures, built without production endian/CRC helpers. |
| Pack evaluator risked owning policy order | Replaced with representability predicates plus raw/effective facts in caller-owned storage. Canonical, operation, validation and Pack own separate order/status/severity/prose/control flow. |
| Transaction regression asserted only model bytes | Expanded to exact result, revision, dirty identity, event, history/redo, retained ID, journal and retry authority. |
| History identity check omitted sprite records/directions | Expanded to collection, frame and sprite-record remove/replace in both directions; unreachable forms remain executable rejection tests. |
| Project golden did not protect parser/admission | Added immutable negative fixtures for version, keys, numeric edges, UTF-8, IDs, references and trailing bytes. |
| Journal fixtures omitted META/CHECKPOINT and could share helpers | Added all four record types plus header/sync/CRC/mixed sequences; production wire helpers are forbidden in fixture construction. |
| One all-family validation fixture was fragile | Replaced with a platform-neutral ordered corpus plus separate platform path cases and multi-entity/cardinality coverage. |
| Cross-platform CLI pack golden ignored absolute paths | Added independent test-root tokenization after asserting absolute form, separators and suffixes; only timings and known root vary. |
| Save result category/outcomes were left open | Chose append-only `file_io_failed` for pre-publication faults and retained `file_durability_uncertain` for authoritative post-publication parent-sync failure. Added an exact phase/outcome table and ownership. |
| Real CLI/GUI parity could not compare random IDs | Required deterministic test RNG or harvested/replayed IDs and a complete shipped mutation-surface manifest. |
| Undo/Redo/session authority lacked one complete oracle | Added an all-operation-family A→B→Undo→A→Redo→B and journal non-publication packet. |
| GUI split lacked state/action trace coverage | Added a pre-split trace oracle and explicit session-shared/action-private/view-local inventory. |
| Recovery split lacked a pre-split fault/state gate | Added coverage gate for pin/no-follow, TOCTOU, claim/live-slot, scan/ranking, quarantine and candidate ownership. |
| D4 had no legal cross-layer include | Chose a legitimate public, engine-free `tp_core` transform contract because D4 is canonical result geometry, not a convenience helper. |
| Recovery OS code could not move verbatim around whole state structs | Added a green seam-formation commit with lock/pin backend values before per-platform moves. |
| `tp_validate_index.c` would be a size-driven utility bucket | Removed it from the target. Indexes remain beside owning rule domains unless reuse is demonstrated. |
| Project split would enlarge an existing catch-all header | Required responsibility-specific path/write/save private headers. |
| Journal `peek/admission` combined unrelated leftovers | Changed target to wire, writer+admission, reader/walk and optional thin peek. |
| Pack constraints packet mixed too many purposes | Split it into oracle/facts, operation migration, Pack migration, validation defect fix, plus a separate animation validation packet. |
| GUI actions split distributed global state without an owner | Added an action-state encapsulation step and required updates to both shipped and headless-test CMake source lists. |
| Filesystem and project both appeared to own publication | Made `tp_project_save.c` the sole staged-sequence owner; filesystem exposes typed OS primitives; session owns the persisted fingerprint. |

## Rejected recommendations

These suggestions appeared in early reviews but conflict with explicit owner
decisions or would make the architecture worse:

- replace hard limits with exact LOC baselines/ratchets/no-growth enforcement;
- make stale ratchet entries fail when a file shrinks;
- stop the simplification audit in favor of immediate feature work;
- split long functions into one-use helpers for lower CC;
- create a generic shared validation-index utility before actual reuse;
- expose whole recovery state structs to backend files;
- treat all diagnostic prose as globally stable;
- merge operation, validation, canonical admission and Pack into one policy.

LOC remains a prominent inventory signal. It is not a build/test gate and does
not determine whether a coherent function or TU must be split.

## Residual risks and checkpoints

- The transient `tp_project` Save failure passed on isolated rerun. The Save
  algorithm was not shown to be incorrect; typed phase/native diagnostics are
  required before its physical split so a recurrence becomes diagnosable.
- Frame-move history records do not encode an independent element identity. The
  revised plan records this as a residual wire risk and forbids an incidental v1
  wire change during simplification.
- macOS filesystem/recovery behavior cannot be verified locally; platform
  backend packets require the complete three-OS matrix before landing.
- Phase 3–5 module shapes are hypotheses. The Phase 2 reassessment may keep a
  large cohesive file or alter a split if the proposed seam increases coupling.
- `scripts/check_boundaries.sh` cannot run in this local Windows environment
  because no WSL distribution is installed; Linux CI remains the executable
  boundary authority.

## Convergence result

No unresolved blocker remains for policy correction, confirmed defect fixes,
contract-oracle construction and shared semantic owners in Phase 0–2. The plan
does not claim later physical splits are ready: each must prove its seam and
oracles in a separate green packet-plan after reassessment.

Second-round results: goal-backward reviewer — `CONVERGED`; ownership/sequencing
reviewer — `CONVERGED`; contract-oracle reviewer — `CONVERGED`.
