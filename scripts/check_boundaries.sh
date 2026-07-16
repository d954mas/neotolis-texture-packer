#!/usr/bin/env bash
# Boundary gates (plan docs/plans/op-layer-and-cli.md §A6): greppable rules that
# keep the tp_core / frontend split honest. Run from the repo root. Exit 0 = clean.
# A legit exception is annotated in-source with "boundary-ok:" on the same line.
set -u
fail=0

say() { printf '%s\n' "$*"; }
hit() {
    fail=1
    say "BOUNDARY VIOLATION [$1]:"
    printf '%s\n' "$2"
}

# Sources under apps/, excluding vendored deps and the selftest (test-internal code).
app_srcs() {
    find apps -type f \( -name '*.c' -o -name '*.h' \) |
        grep -v '/deps/' |
        grep -vE '/(gui_selftest|test_[^/]*|tp_bench_[^/]*)\.(c|h)$'
}

# Shipping app/core sources only. Documentation, fixtures, unit tests, spikes,
# benchmarks, and the GUI selftest oracle are deliberately outside deletion
# gates: they may name a retired path while proving migration compatibility.
shipping_srcs() {
    app_srcs
    find packer/include/tp_core packer/src -type f \
        \( -name '*.c' -o -name '*.h' \)
}

# 1. No sprite-name extension stripping outside tp_core (tp_sprite_export_key is
#    the single owner). Project-FILENAME helpers must carry a boundary-ok note.
r1=$(app_srcs | xargs grep -nE "strrchr\([^,]+, *'\.'\)" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r1" ] && hit "R1 ext-strip outside tp_core" "$r1"

# 2. No exporter-id string literals in frontends (use TP_EXPORTER_ID_* / registry).
r2=$(app_srcs | xargs grep -nE '"(json-neotolis|defold)"' 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r2" ] && hit "R2 exporter-id literal in frontend" "$r2"

# 3. No pack-desc assembly in frontends (tp_pack_input_build owns encoding).
r3=$(app_srcs | xargs grep -nE 'ov_mask|TP_PACK_OV_' 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r3" ] && hit "R3 desc assembly in frontend" "$r3"

# 4. Public tp_core headers name no engine types (comments stripped first).
r4=$(for f in packer/include/tp_core/*.h; do
    sed -e 's|/\*.*\*/||g' -e 's|//.*||' -e 's|/\*.*||' -e '/^[[:space:]]*\*/d' "$f" |
        grep -nE '\bnt_[a-z_]+_t\b' | sed "s|^|$f:|"
done)
[ -n "$r4" ] && hit "R4 nt_* type in public tp header" "$r4"

# 5. AGENTS.md may not present apps/cli as existing while the directory is absent
#    (an "(in progress" qualifier on the same line is the sanctioned interim state).
if [ ! -d apps/cli ]; then
    r5=$(grep -n 'apps/cli' AGENTS.md | grep -v 'in progress')
    [ -n "$r5" ] && hit "R5 AGENTS.md describes apps/cli which does not exist" "$r5"
fi

# 6. CLI mutation cutover (F2-05a): apps/cli/cli_mutate.c routes every mutating verb
#    through the typed operation/transaction engine (tp_operation + tp_model_apply). It
#    must NOT hand-mutate the loaded project -- neither by calling the inline tp_project_*
#    mutators the ops replaced (R6a), nor by assigning into the loaded project's arrays
#    directly (R6b), nor through an alias into it (R6c). do_new's tp_project_create +
#    tp_project_atlas_seed_default_target (project
#    lifecycle, not a mutation op) are deliberately NOT in the banned set. A legit
#    exception carries a "boundary-ok:" note on the same line.
_cutover="apps/cli/cli_mutate.c"
# The inline project mutators the F2-05a ops replaced (op-payload field WRITES reuse the
# same field NAMES, so a blanket field-name ban would false-positive: the reliable proof
# of the cutover is that NONE of these mutators are called + a write into p->atlases[]).
_muts='tp_project_(add_atlas|remove_atlas|set_atlas_name|atlas_add_source|atlas_add_source_kind|atlas_remove_source|atlas_add_sprite|atlas_remove_sprite|atlas_prune_sprite|atlas_set_sprite_rename|atlas_add_animation|atlas_remove_animation|anim_add_frame|anim_remove_frame|anim_move_frame|atlas_add_target|atlas_remove_target|atlas_set_target)\('
_projwrite='p->atlases\[[^]]*\]\.[A-Za-z_]+[[:space:]]*=[^=]'
# R6c bans the SAME in-place write reached through an ALIAS into the loaded project.
# cli_mutate holds `tp_project_atlas *a = &p->atlases[ai]` (and the `t` target / `an`
# animation sub-entity aliases); a reintroduced in-place `a->max_size = 512;` would slip
# past R6b's literal `p->atlases[...]` match, defeating the guard. Ban assignment through
# those alias names. Op-building writes all go through differently-named locals (`op`,
# `s`, `e`), and alias READS (`tp_id128 aid = a->id;`) have no `=` after the field, so
# this does not false-positive.
_aliaswrite='(^|[^A-Za-z0-9_])(a|an|t)->[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[^=]'

if [ -f "$_cutover" ]; then
    r6a=$(grep -nE "$_muts" "$_cutover" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$r6a" ] && hit "R6a inline project mutator in cli_mutate (use a tp_operation)" "$r6a"
    r6b=$(grep -nE "$_projwrite" "$_cutover" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$r6b" ] && hit "R6b direct write into the loaded project in cli_mutate (build an op)" "$r6b"
    r6c=$(grep -nE "$_aliaswrite" "$_cutover" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$r6c" ] && hit "R6c write through a loaded-project alias in cli_mutate (build an op)" "$r6c"

    # Self-test: prove the R6 detectors actually FIRE on a seeded violation (fail closed if
    # a future edit breaks the regex -- so "a seeded boundary violation is caught" is
    # asserted on every run, not just assumed).
    if ! printf '    tp_project_atlas_add_sprite(a, "x", &s);\n' | grep -qE "$_muts"; then
        hit "R6-selftest" "R6a detector failed to catch a seeded inline-mutator violation"
    fi
    if ! printf '    p->atlases[0].max_size = 512;\n' | grep -qE "$_projwrite"; then
        hit "R6-selftest" "R6b detector failed to catch a seeded project-write violation"
    fi
    if ! printf '    a->max_size = 512;\n' | grep -qE "$_aliaswrite"; then
        hit "R6-selftest" "R6c detector failed to catch a seeded aliased-write violation"
    fi
    # ...and does NOT fire on the legitimate op-payload / lifecycle / alias-READ forms.
    if printf '    op.u.atlas_settings.max_size = iv;\n    tp_project_create();\n    tp_id128 aid = a->id;\n' | grep -qE "$_muts|$_projwrite|$_aliaswrite"; then
        hit "R6-selftest" "R6 detector false-positives on a legitimate op-payload / lifecycle / alias-read line"
    fi
fi

# 7. GUI mutation cutover (F2-05b-i): the GUI mutation-surface files route every model
#    change through the typed operation/transaction engine (gui_project.c builds
#    tp_operation(s) + commits via tp_model_apply; the view/declare fns ENQUEUE via
#    gui_edit_* and never mutate). Like the CLI's R6, they must NOT hand-mutate the loaded
#    project -- not by calling the inline tp_project_* mutators the ops replaced (R7a), nor by
#    assigning into the loaded project's arrays (R7b), nor through an alias into it (R7c).
#    Scoped to the mutation-surface TUs (mirrors R6's cli_mutate.c scope). gui_selftest.c (a
#    boundary-excluded dev-seam test harness that pokes internals -- like the CLI's tests) and
#    main.c (app shell + the --parity dev seam) are out of scope, as is the SANCTIONED set:
#    tp_project_atlas_seed_default_target + tp_project_promote_ids (lifecycle, not a mutation
#    op). (H/P1-2 retired the former animation-rename direct-write exception: animation rename
#    now routes through TP_OP_ANIMATION_RENAME.) A legit exception carries a "boundary-ok:" note
#    on the same line.
_gui_muts="apps/gui/gui_project.c apps/gui/gui_view_settings.c apps/gui/gui_view_lists.c apps/gui/gui_view_chrome.c apps/gui/gui_actions.c"
# The inline project mutators the F2-05b-i ops replaced (as R6, seed_default_target +
# promote_ids are lifecycle, NOT banned).
_gmuts='tp_project_(add_atlas|remove_atlas|set_atlas_name|atlas_add_source|atlas_add_source_kind|atlas_remove_source|atlas_add_sprite|atlas_remove_sprite|atlas_prune_sprite|atlas_set_sprite_rename|atlas_add_animation|atlas_remove_animation|anim_add_frame|anim_remove_frame|anim_move_frame|atlas_add_target|atlas_remove_target|atlas_set_target)\('
_gprojwrite='(p|proj)->atlases\[[^]]*\]\.[A-Za-z_]+[[:space:]]*=[^=]'
# R7c bans the SAME in-place write through an ALIAS into the loaded project (a/an/t/ov the
# declare fns hold). Op-payload writes go through differently-named locals (op, s/p payload,
# e), and alias READS (tp_id128 aid = a->id;) have no `=` after the field.
_galiaswrite='(^|[^A-Za-z0-9_])(a|an|t|ov)->[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[^=]'

for _f in $_gui_muts; do
    [ -f "$_f" ] || continue
    g7a=$(grep -nE "$_gmuts" "$_f" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$g7a" ] && hit "R7a inline project mutator in $_f (use a tp_operation)" "$g7a"
    g7b=$(grep -nE "$_gprojwrite" "$_f" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$g7b" ] && hit "R7b direct write into the loaded project in $_f (build an op)" "$g7b"
    g7c=$(grep -nE "$_galiaswrite" "$_f" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$g7c" ] && hit "R7c write through a loaded-project alias in $_f (build an op)" "$g7c"
done

# Self-test: prove the R7 detectors fire on a seeded violation and do NOT false-positive on
# the legitimate op-payload / lifecycle / alias-READ forms (asserted every run).
if ! printf '    tp_project_atlas_add_sprite(a, "x", &s);\n' | grep -qE "$_gmuts"; then
    hit "R7-selftest" "R7a detector failed to catch a seeded inline-mutator violation"
fi
if ! printf '    proj->atlases[0].padding = 7;\n' | grep -qE "$_gprojwrite"; then
    hit "R7-selftest" "R7b detector failed to catch a seeded project-write violation"
fi
if ! printf '    a->padding = 7;\n' | grep -qE "$_galiaswrite"; then
    hit "R7-selftest" "R7c detector failed to catch a seeded aliased-write violation"
fi
if printf '    op.u.atlas_settings.padding = iv;\n    tp_project_atlas_seed_default_target(p, idx);\n    tp_id128 aid = a->id;\n' | grep -qE "$_gmuts|$_gprojwrite|$_galiaswrite"; then
    hit "R7-selftest" "R7 detector false-positives on a legitimate op-payload / lifecycle / alias-read line"
fi

# 8. A shipping frontend must not create/borrow mutable model/project authority.
#    Reads use immutable session snapshots; mutation/persistence goes through
#    tp_session. There is no read-only tp_project exemption.
_frontend_authority='(^|[^A-Za-z0-9_])(tp_model[[:space:]]*\*|tp_model_[A-Za-z0-9_]*\(|tp_project[[:space:]]*\*|tp_project_(load|create|destroy|get_atlas|save|save_with_fingerprint|save_if_unchanged|add_atlas|remove_atlas|set_atlas_name|atlas_[A-Za-z0-9_]*)\(|tp_identity_file_fingerprint\()'
r8=$(app_srcs | grep -vE '/(test_[^/]*|tp_bench_[^/]*)\.(c|h)$' |
    xargs grep -nE "$_frontend_authority" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r8" ] && hit "R8 mutable model/direct save in shipping frontend (use tp_session)" "$r8"
for _seed in \
    '    tp_model *m;' \
    '    tp_model_can_redo(m);' \
    '    tp_project *p;' \
    '    tp_project_load(path, &p, &err);' \
    '    tp_project_get_atlas(p, 0);' \
    '    tp_project_save(p, path, &err);' \
    '    tp_project_save_with_fingerprint(p, path, &fp, &err);' \
    '    tp_project_save_if_unchanged(p, path, fp, &err);' \
    '    tp_identity_file_fingerprint(path, &fp, &err);'
do
    if ! printf '%s\n' "$_seed" | grep -qE "$_frontend_authority"; then
        hit "R8-selftest" "R8 detector failed to catch seeded authority: $_seed"
    fi
done
if printf '    tp_session *s = NULL;\n    tp_session_save(s, &result, &err);\n    tp_project_save_buffer(p, &bytes, &length, &err);\n    const tp_snapshot_atlas *a = view;\n' |
    grep -qE "$_frontend_authority"; then
    hit "R8-selftest" "R8 detector false-positives on session authority / read-only serialization / immutable DTOs"
fi

# 9. The GUI may present recovery choices and map typed results, but it must not
#    own the recovery store/live slot/claim or construct journal I/O. Those
#    lifetimes belong to the shared recovery/session boundary.
_gui_recovery_owner='(^|[^A-Za-z0-9_])(tp_recovery_(domain|store|live|claim)[[:space:]]*\*|tp_recovery_(domain|store|live|claim)_[A-Za-z0-9_]*\(|tp_journal_(create|destroy|io_[A-Za-z0-9_]*)\()'
r9=$(find apps/gui -type f \( -name '*.c' -o -name '*.h' \) |
    grep -vE '/(gui_selftest|test_[^/]*|tp_bench_[^/]*)\.(c|h)$' |
    xargs grep -nE "$_gui_recovery_owner" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r9" ] && hit "R9 recovery storage ownership in GUI (use shared recovery flow)" "$r9"
if ! printf '    tp_recovery_domain *domain;\n    tp_recovery_store *store;\n    tp_journal_io_file(path);\n' |
    grep -qE "$_gui_recovery_owner"; then
    hit "R9-selftest" "R9 detector failed to catch seeded GUI recovery ownership"
fi
if printf '    tp_recovery_candidates list;\n    tp_recovery_resolution *choice;\n' |
    grep -qE "$_gui_recovery_owner"; then
    hit "R9-selftest" "R9 detector false-positives on recovery presentation/result DTOs"
fi

# 10. tp_session is orchestration only. It may call public recovery/lease APIs,
#     but may not depend on a frontend/protocol or contain a recovery codec,
#     filesystem/lock backend, Pack, or Export implementation.
_session_deps='#include[[:space:]]*[<"][^>"]*(apps/|gui|cli|protocol|cJSON)'
_session_impl='(^|[^A-Za-z0-9_])(fopen|fwrite|open|CreateFile|LockFile|tp_journal_(encode|decode)[A-Za-z0-9_]*|tp_pack|tp_export_run)[[:space:]]*\('
r10a=$(grep -nE "$_session_deps" packer/src/tp_session.c packer/src/tp_session_internal.h 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r10a" ] && hit "R10a frontend/protocol dependency in tp_session" "$r10a"
r10b=$(grep -nE "$_session_impl" packer/src/tp_session.c packer/src/tp_session_internal.h 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r10b" ] && hit "R10b backend/codec/Pack/Export implementation in tp_session" "$r10b"
if ! printf '#include "apps/gui/gui_project.h"\n' | grep -qE "$_session_deps"; then
    hit "R10-selftest" "R10a detector failed to catch seeded GUI dependency"
fi
if ! printf '    FILE *f = fopen(path, "rb");\n    tp_pack(input, &out, &err);\n' |
    grep -qE "$_session_impl"; then
    hit "R10-selftest" "R10b detector failed to catch seeded backend/job implementation"
fi
if printf '#include "tp_core/tp_recovery.h"\n    tp_project_lease_acquire(path, &lease, &err);\n    tp_session_snapshot_create(s, &snapshot, &err);\n' |
    grep -qE "$_session_deps|$_session_impl"; then
    hit "R10-selftest" "R10 detector false-positives on allowed public orchestration calls"
fi

# 11. GUI source refresh has one publication choke point. The scan backend may
#     clear its own cache, but shipping callers must also advance the session
#     source generation/event through gui_project_invalidate_sources().
r11=$(find apps/gui -type f \( -name '*.c' -o -name '*.h' \) |
    grep -vE '/(gui_project|gui_scan|gui_selftest|test_[^/]*|tp_bench_[^/]*)\.(c|h)$' |
    xargs grep -nE 'gui_scan_invalidate_all[[:space:]]*\(' 2>/dev/null)
[ -n "$r11" ] && hit "R11 raw GUI source invalidation outside project chokepoint" "$r11"
if ! printf '    gui_scan_invalidate_all();\n' | grep -qE 'gui_scan_invalidate_all[[:space:]]*\('; then
    hit "R11-selftest" "R11 detector failed to catch a seeded raw invalidation"
fi

# 12. Deferred collection intents capture stable IDs + expected revision, never
#     a mutable collection index. A sanctioned non-entity option carries the
#     normal boundary-ok annotation.
_queued_index='(^|[^A-Za-z0-9_])int[[:space:]]+s_pending_[A-Za-z0-9_]*(atlas|source|anim|target|selection)[[:space:]]*(=|;)'
r12=$(grep -nE "$_queued_index" apps/gui/gui_actions.c apps/gui/gui_actions.h 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r12" ] && hit "R12 queued GUI collection index (capture stable ID + revision)" "$r12"
if ! printf '    int s_pending_browse_target = -1;\n' | grep -qE "$_queued_index"; then
    hit "R12-selftest" "R12 detector failed to catch a seeded queued target index"
fi

# 13. gui_pack is a thin adapter over the session-owned typed runtime handle.
#     Worker/thread/atomic job authority belongs to tp_build (tp_job.c), never
#     to the frontend again. Synchronous selftest helpers must drain this same
#     typed path; direct input/settings assembly or Pack/Export algorithms are
#     not a sanctioned second route.
_gui_job_owner='(^|[^A-Za-z0-9_])(thrd_(create|join)|atomic_[A-Za-z0-9_]*|_Atomic|pack_worker|export_worker|s_job(_active)?|tp_pack[[:space:]]*\(|tp_pack_input_build[A-Za-z0-9_]*[[:space:]]*\(|tp_pack_settings_build[A-Za-z0-9_]*[[:space:]]*\(|tp_export_snapshot_job_[A-Za-z0-9_]*[[:space:]]*\(|tp_pack_sprite_desc)([^A-Za-z0-9_]|$)'
r13=$(grep -nE "$_gui_job_owner" apps/gui/gui_pack.c 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r13" ] && hit "R13 GUI owns Pack/Export worker state (use tp_session job API)" "$r13"
if ! printf '    static _Atomic int s_job_active;\n    thrd_create(&thread, pack_worker, ctx);\n    tp_pack(&settings, arena, &result, &err);\n' |
    grep -qE "$_gui_job_owner"; then
    hit "R13-selftest" "R13 detector failed to catch seeded GUI job authority"
fi
if printf '    tp_session_pack_job_start(session, &request, &err);\n    tp_session_job_poll(session, &progress, &err);\n' |
    grep -qE "$_gui_job_owner"; then
    hit "R13-selftest" "R13 detector false-positives on typed job orchestration"
fi

# 14. Core semantic-diff admission is the single no-change owner. The deleted
#     GUI action tags and pending_is_noop mirror must not return under another
#     maintenance pass; the adapter submits typed intent and observes revision.
_gui_noop_owner='(^|[^A-Za-z0-9_])(pending_is_noop|gui_action|GUI_ACT_[A-Za-z0-9_]+)([^A-Za-z0-9_]|$)'
r14=$(grep -nE "$_gui_noop_owner" apps/gui/gui_project.c apps/gui/gui_project.h 2>/dev/null)
[ -n "$r14" ] && hit "R14 GUI duplicates semantic no-change ownership" "$r14"
if ! printf '    static bool pending_is_noop(void);\n' | grep -qE "$_gui_noop_owner"; then
    hit "R14-selftest" "R14 detector failed to catch seeded GUI no-op ownership"
fi
if printf '    refresh_after_session_commit();\n' | grep -qE "$_gui_noop_owner"; then
    hit "R14-selftest" "R14 detector false-positives on thin post-commit projection"
fi

# 15. Architecture-foundation deletion gate. These identifiers belonged to
#     superseded authoritative paths and must not return to shipping app/core
#     code. Test/selftest code may retain an oracle with the old spelling, but
#     production has no compatibility exception or boundary-ok escape hatch.
_retired_foundation_symbols='(^|[^A-Za-z0-9_])(GEDIT_[A-Za-z0-9_]*|s_refresh_epoch|s_pack_start_refresh_epoch|s_preview_ver|model_generation_at_start|model_changed_since|TP_CLIENT_CAPABILITY_LIVE_JOBS)([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])(gui_project_get|gui_pack_find_sprite)[[:space:]]*\('
r15=$(shipping_srcs | xargs grep -nE "$_retired_foundation_symbols" 2>/dev/null)
[ -n "$r15" ] && hit "R15 retired foundation path in shipping source" "$r15"

for _seed in \
    '    GEDIT_ATLAS_RENAME,' \
    '    gui_project_get();' \
    '    gui_pack_find_sprite(0, "hero");' \
    '    ++s_refresh_epoch;' \
    '    s_pack_start_refresh_epoch = s_refresh_epoch;' \
    '    s_preview_ver = generation;' \
    '    result.model_generation_at_start = generation;' \
    '    model_changed_since = generation != start_generation;' \
    '    TP_CLIENT_CAPABILITY_LIVE_JOBS'
do
    if ! printf '%s\n' "$_seed" | grep -qE "$_retired_foundation_symbols"; then
        hit "R15-selftest" "R15 detector failed to catch retired symbol: $_seed"
    fi
done
if printf '    gui_pack_find_sprite_ref(0, source_id, key);\n    tp_session_snapshot_model_generation(snapshot);\n    TP_CLIENT_CAPABILITY_PACK_JOB\n' |
    grep -qE "$_retired_foundation_symbols"; then
    hit "R15-selftest" "R15 detector false-positives on canonical foundation APIs"
fi

# 16. The measured arena transaction-clone prototype never acquired a production
#     caller and was retired with its benchmark. A future isolated benchmark may
#     revisit immutable snapshots, but shipping code keeps one clone owner until
#     profiling selects a replacement. GUI private-core visibility is selftest-only.
_retired_clone_symbols='(^|[^A-Za-z0-9_])(tp_project_clone_arena_footprint|tp_project_clone_into_arena)([^A-Za-z0-9_]|$)'
r16a=$(shipping_srcs | xargs grep -nE "$_retired_clone_symbols" 2>/dev/null)
[ -n "$r16a" ] && hit "R16a retired arena project clone returned" "$r16a"

gui_private_scope_counts() {
    awk '
        /if[[:space:]]*\([[:space:]]*NTPACKER_GUI_SELFTEST[[:space:]]*\)/ {
            selftest = 1
        }
        !in_include && /target_include_directories[[:space:]]*\(/ {
            in_include = 1
            include_command = ""
            include_selftest = selftest
        }
        in_include {
            include_command = include_command " " $0
            if ($0 ~ /\)/) {
                if (include_command ~ /ntpacker-gui/ &&
                    include_command ~ /packer\/src/) {
                    total++
                    if (include_selftest) scoped++; else outside++
                }
                in_include = 0
            }
        }
        selftest && /^[[:space:]]*endif[[:space:]]*\(/ {
            selftest = 0
        }
        END { printf "%d %d %d", total, scoped, outside }
    '
}
gui_private_scope=$(gui_private_scope_counts < apps/gui/CMakeLists.txt)
[ "$gui_private_scope" != "1 1 0" ] &&
    hit "R16b shipping GUI private-core include exposure" \
        "expected total/selftest/outside = 1 1 0, found $gui_private_scope"

if ! printf '    tp_project_clone_into_arena(project, arena);\n' |
    grep -qE "$_retired_clone_symbols"; then
    hit "R16-selftest" "R16a detector failed to catch the retired arena clone"
fi
seeded_private_scope=$(printf '%s\n' \
    'target_include_directories(ntpacker-gui PRIVATE "${CMAKE_SOURCE_DIR}/packer/src")' |
    gui_private_scope_counts)
if [ "$seeded_private_scope" != "1 0 1" ]; then
    hit "R16-selftest" "R16b scope detector missed an unconditional GUI private include"
fi
seeded_multiline_scope=$(printf '%s\n' \
    'target_include_directories(ntpacker-gui PRIVATE' \
    '    "${CMAKE_SOURCE_DIR}/packer/src")' |
    gui_private_scope_counts)
if [ "$seeded_multiline_scope" != "1 0 1" ]; then
    hit "R16-selftest" "R16b scope detector missed a multiline unconditional GUI private include"
fi

if [ "$fail" -eq 0 ]; then
    say "boundaries OK"
fi
exit "$fail"
