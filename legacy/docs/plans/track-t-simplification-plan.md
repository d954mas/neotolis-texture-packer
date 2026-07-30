# Track T v2 — full-project simplification plan (revised after 4-way critique)

v1 was drafted from four architectural surveys; it was then adversarially
critiqued by three independent reviewers (feasibility, value/YAGNI,
spec-conformance) and by Codex CLI (independent repo analysis + per-packet
verdicts). This v2 keeps only what survived. Honest projection: **−1.5k…−2.5k
LOC** (v1's −3.5k…−5k did not survive audit; S16/S17 history shows
"N untyped mechanisms → one typed mechanism" packets land net-positive).

Standing constraints: worker process boundary stays; no auto-pack; lag-free
big atlas; five frozen oracles byte-identical (incl. USA-20 tag adjacency at
test_gui_action_trace_draft.c:544); test deletion only with named successor;
errors are structured data, not prose; fix roots, never work around.
"Contracts may change freely" applies to implementation-private forms only —
normative behavior, wire/JSON schemas, stable tokens, frozen oracles need an
explicit owner/spec decision.

## OWNER FORKS — ALL RESOLVED (2026-07-29)

F1 — Session gate: OWNER CHOSE (a) — delete the gate, amend
docs/design/ui-session-architecture-spec.md (the "defensive synchronization"
sentence and the "under one session gate" phrasing move to the owner-thread
model), replace with an owner-thread NT_ASSERT stored at session create.
Dev API/MCP enqueue to the one thread per master spec §4.8.
F2 — R22 redesign: conservative port (Option B). Runtime TP_USA is dead
(cannot enter frozen USA-20/19 oracle bodies).
F3 — cache cold tier: LANDED as S28 (miniz deflate L6, background encode,
fork-join decode, 512MiB compressed+meta budget) @ 533b649+cd8335b.
W1.3 DESIGN RESOLVED — unified app scratch root: both GUI and CLI resolve
<app cache dir>/ntpacker/work (LOCALAPPDATA / XDG_CACHE_HOME; resolver
shared via apps/common) and every job gets req-<pid>-<reqid>/ under it.
The GUI's legacy <exe_dir>/pack_session convention is DELETED (read-only
under Program Files, shared across instances); the CLI moves off raw
%TEMP% (Linux /tmp is RAM-backed tmpfs — a 604MB artifact would eat RAM;
Windows cleaners can wipe mid-job). Export jobs get the same req- dir as
Pack (fixes the cross-process .ntpack clobber, spec §10.4).
W2-12 RESOLVED — current_pack_input_hash wire field: DELETE. U-04 freshness
recomputes hashes off-UI-thread on its own cadence (a completion-time
snapshot cannot serve later staleness checks); named test successors
required for the fields' assertions.
WAVE 1 START: APPROVED by owner 2026-07-29, including the session-gate
deletion packet (F1a).

## WAVE 1 — COMPLETE (2026-07-30), CI green

All seven items plus the F1a gate deletion landed: 03c1988 (3), 7f41e83
(6), 2f8e365 (2), 2e871c6 (1+7), 9904dcb (4), fb0a5c9 (F1a), 33f8901 (5).
Battery moved 160/159 -> 169/168 (app_scratch contract test + 8 gate
fixtures). Three items differed from the text below and the difference is
the finding, not a deviation to excuse:

- Item 4 was written as a mode switch; converting the scans exposed that
  the gate's lexer stripped comments BEFORE blanking strings, so a `'"'`
  character literal blanked ~40 real lines and hid two system() calls
  from every whole-file rule. Comments and literals are one lexical layer
  now, and the include scan (which must keep strings) aborts if its
  directive count disagrees with the correctly lexed text.
- Item 4's "expand the VIEW_PLATFORM allowances to full call lists" was a
  no-op: that allowance format is per-symbol and already complete. The
  stale doc comment was corrected instead.
- Item 5 said "extend A6". A6 lives in the cmake gate and pins two NAMED
  seams to their fences; the new R24 in the shell gate is its complement
  (no unfenced seam anywhere in shipping code), not a second copy. The
  two gates merge under wave-4 item 19.
- Item 3's design note undercounted: 25 tp_project__test_* declarations,
  not 22 (three clone seeds are declared in tp_txn_internal.h).

Wave-2 backlog opened by wave 1 (fix these where item 9 lands):
tp_core_seams library so no image mixes fenced and unfenced TUs;
tp_export.h names a seam in a PUBLIC header; tp_sb.allocation_count is a
test probe in a public struct that R24 cannot see; tp_error_set does not
guarantee well-formed UTF-8 at the producer; the duplicated GUI hex-id
grammars; req- dir reaping never runs in a CLI-only environment.

## WAVE 1 — defects (small, start immediately)

1. Locale-dependent folding in tp_project_path.c: tolower at :156 AND
   isalpha at :36/:109 (drive-letter classification) → shared ASCII fold /
   explicit range checks. Determinism, spec §4.3. Add a one-line gate rule
   banning locale ctype outside the shared fold (lands green).
2. Inner build proto: validate_text (UTF-8 + NUL + cap) at tp_build_proto
   decode, so a builder message can never make the outer codec refuse the
   whole terminal frame.
3. Export req- dir for run_export (spec §10.4 violation; S24 did NOT fix
   it): one private request dir per export job, exactly like run_pack.
4. Architecture gate: MATCHALL for the three call scans + the declaration
   scan + HOST_QUEUE scan (S25b did includes only). Same commit must expand
   the VIEW_PLATFORM debt allowances to full call lists and fix/allowance
   any newly unmasked hard-rule hit. (T9 later subsumes this; deliberate
   double-spend, recorded.)
5. Fence the test surface: 5 tp_session__test_* + generation seam moved
   under TP_ENABLE_TEST_SEAMS; tp_build_worker_opts split into production
   config vs test controls (per Codex); classify the 22 tp_project__test_*
   declarations; extend A6 to flag __test_ outside a fence.
6. Typed admission rejections: wire OLD_INSTANCE + SESSION_CLOSED at the two
   sites that already reject with prose (gui_host_queue.c:247,:531); delete
   SUPERSEDED + DUPLICATE from the PUBLIC enum (no spec producer owed);
   keep the internal admission enum. Must not touch the accepted-result
   projection (stale-result isolation, spec §10.3).
7. FNV hygiene: fix the dropped-digit basis in the 4 in-memory sites +
   add a separator byte between id and key concatenations. Verify none is
   persisted or oracle-pinned first. Keep gui_crash's CRT-free copy.
   (T0.2 hex "unification" is DROPPED — different grammars by design.)

## WAVE 2 — cheap, high leverage

8. CI: shared Linux ccache key, ctest -j (after auditing fixed fs names),
   timeout-minutes per job, ccache in release.yml, wire
   report_loc_inventory as a non-gating CI step (deleting it is prohibited
   by AGENTS) and fix its dev/test misclassification. Consolidate the three
   full-CLI fault clones into one binary with runtime-selected seams if the
   seam model allows, else two. Skip-architecture-repeats waits for the
   gate consolidation.
9. Scratch substrate (finish the S24 extraction): one exclusive-create +
   owner-parse + reap + remove_tree family shared by pkw-/req-/.tp-stage-;
   keep distinct prefixes and lifetimes (pkw- worker-owned, req- host-owned,
   .tp-stage- publication). Closes the pkw-/req- check-then-create races.
   Verify the two remove_tree buffer macros are equal before merging.
10. Wire substrate: one frame header + bounded cursor under both codecs
    (byte-compatible; keeps versions, caps, message sets, fault semantics);
    build request framing switches to exact header-declared reads (delete
    the growable EOF read); evaluate the response side against its
    reply_cap seam — share only what is genuinely equivalent. The win32
    blocking-over-nonblocking unification is DROPPED (documented anti-goal:
    named-pipe namespace only for the outer worker).
11. Terminal authority: the immutable completion envelope is the sole
    state/status/error authority; result becomes typed payload; delete the
    gui_pack_jobs fallback reconstruction. (Spec §4.8 keeps the retained
    envelope — this implements it, not collapses it.)
12. Dead wire fields with named test successors: freshness 3→1 (keep
    freshness_status as the PROBE_ERROR cause carrier — it has a live
    worker read; drop the second tp_error), drop current_pack_input_hash
    ONLY IF owner confirms U-04 will recompute worker-side (it is today's
    only off-UI-thread current hash — flag to owner), dedupe the token
    (rename, not deletion). RESPONSE_PACK_BYTES 132→96 (or 80 with token
    dedupe). Keep: builder_code (engine #304 lands into it), client
    capability, cli_verb, side_effect_coordinator (Extract seam),
    events_after deletion PROCEEDS (spec-advancing; successor = observation
    events/resync; 9 test call sites migrate).

## WAVE 3 — GUI structure (mostly clarity; sell as perf where true)

13. gui_pack keys: int atlas_index → tp_id128 (~178 sites incl. selftest 75;
    the selftest macro block at gui_selftest.c:658-671 must be DELETED IN
    THE SAME PACKET — its index-first macros would mis-bind, not fail).
    Kills the stacked O(n) scans; spec stable-identity rule.
14. One canvas binding resolved once per frame (new gui_canvas_binding();
    today 3-4 independent residency-mutating resolutions — the S20/S21 P1
    class). Frame phases: extract the 5 phase functions from frame()
    (no runtime enum; // #region + function boundaries; the ordering
    contracts move into function names and NT_ASSERTs only where an ingress
    is genuinely reachable out of order).
15. Context-menu payload struct (S16 shape) + intent group-kind
    canonicalization (prove slice9 group behavior identical — USA-20
    neighborhood). gui_state.h: real owner accessors for the ~10 globals
    views WRITE (modal/preview/pack flags); style/ids stay as-is; widen A7.
16. gui_selftest: delete macro block (see 13), add // #region phases, split
    run_selftest into phase functions; retarget the 5 synchronous
    create_animation_from_selection call sites onto the async intent path,
    then delete the duplicate flow. gui_rows.c split ONLY along the
    ownership seams its 7 regions already mark (5 owners is an ownership
    split, not a metric split), keeping the bench counters intact.
    Shutdown loop moves to the host-lifecycle owner (becomes testable).

## WAVE 4 — big packets, each with its own design doc before execution

17. GUI scale path (NEW, from Codex + owner's top priority; feeds U-04):
    generation-keyed stable-ID lookup replacing linear identity scans in
    gui_rows; budgeted/incremental page residency replacing the synchronous
    drop-and-upload-all in gui_canvas_resources (16-page hard cap vs spec
    30-atlas/5000-sprite <100ms); benchmark the four interactions at spec
    scale. Combines with F3's compressed cold tier.
18. Clone/diff unification (NEW, from Codex): tp_project_clone.c +
    tp_diff_entity.c = 872 LOC of deliberately mirrored per-field copying
    where a missed field silently diverges Undo. Allocator-parameterized
    owned-entity copy helpers; keep both fault-injection contexts; Undo
    round-trip oracles (§9.5 byte-identical A) as acceptance.
19. Boundary gate consolidation (T9 revised): first the cheap fix — batch
    the ~90 grep spawns in check_boundaries.sh (it is spawn-bound: 12.7s
    sys of 22.9s); then, if still wanted, one purpose-built scanner under
    ctest with the G1-G7 migration guardrails (parity ctest comparing
    normalized {rule,file,line,symbol} hit sets; sh authoritative per
    family until ported; final commit pure deletion) and R22 Option B.
    Follow-up packet: retire the view-layer debt allowances (ports/
    projections so views stop reaching platform APIs and pack results).
20. Session ownership split (BLOCKED on F1): separate admission/events,
    persistence/lease/fingerprint, history projection out of tp_session.c
    (1383 LOC, ~59 fns); gate fate per F1. The _locked twin ceremony goes
    either way. tp_pack() and the 12-16 public _ex twins: audit one by one;
    delete only with a canonical replacement (tp_pack -> tp_pack_cancellable
    NULL is documented); the 129 test call sites are the cost — do it where
    the header prose gets simpler, skip where it just adds NULL args.
21. Comment rewrite (T11 slice, mandated by engine AGENTS): convert the
    ~52-67 history-citing comments into stated invariants (keep decision
    numbers where a rule is also stated); extend R17 to gate the
    "decision NNNN" bare form. Individually-justified T11 pieces: generate
    tp_status enum from TP_STATUS_LIST (stable explicit values), one
    byte-cursor for journal/history codecs. DROPPED from T11: universal
    collector (three deliberately opposite ownership contracts; malloc in
    frame loop), header folds (fan-out risk + R18 churn for ~40 lines),
    renames (blame churn; tp_project_identity DOES contain identity logic).

## DROPPED (with reasons, so they stay dropped)

- T2 registry-driven project writer/parser: byte-pinned golden
  (project_v5_rich.golden, EQUAL_MEMORY), interleaved ASCII key order,
  joint sparse predicates (origin 2→1, slice9 4→1, enabled-only-false,
  "unlimited" token), writer/parser independence invariant (impl-plan
  §19.2), S17's explicit carve-out. Registry stays the schema authority
  for CLIENTS; the file codec stays hand-written. Revisit only with a
  consumer-by-field coverage matrix after S26 settles.
- tp_primitives grow/strdup normalization: 26-28 heterogeneous sites, 8 cap
  seeds, 4 NULL contracts, 4 OOM seams; two existing generic helpers were
  not adopted — genericity was never the blocker. Take only: tp_ascii fold,
  tp_hex extension, the two near-identical component lexers, dup_cstr NULL
  collision, the two identical GUI hex encoders (→ apps/common).
- Serialized cache mode as built (premise measured false: blob is raw).
- Win32 blocking-proc unification; envelope/result collapse (both halves
  keep spec roles); T0.2 hex unification; universal diagnostic collector;
  header folds; renames; runtime frame-phase enum; TP_USA runtime tags.

## Order

W1 (all independent, small commits) → W2 → W3 (13+16-macro first, then 14,
15, 16) → W4 packets each gated on its design note + owner sign-off where
marked. The cache cold tier (F3) and GUI scale path (17) together form the
perf track and can proceed in parallel with W1-W3.
