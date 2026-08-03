#!/usr/bin/env bash
# Boundary gates: greppable rules that keep the tp_core / frontend split honest.
# Run from the repo root. Exit 0 = clean.
# A legit exception is annotated in-source with "boundary-ok:" on the same line.
#
# Native Windows CTest can launch Git Bash with a PATH that does not contain
# Bash's own /usr/bin directory. Establish it using shell parameter expansion
# before the gate calls grep/sed/awk/mktemp.
case "${BASH:-}" in
    */*)
        _tp_bash_bin=${BASH%/*}
        case ":${PATH:-}:" in
            *":$_tp_bash_bin:"*) ;;
            *) PATH="$_tp_bash_bin${PATH:+:$PATH}" ;;
        esac
        export PATH
        unset _tp_bash_bin
        ;;
esac

set -u
fail=0

say() { printf '%s\n' "$*"; }
hit() {
    fail=1
    say "BOUNDARY VIOLATION [$1]:"
    printf '%s\n' "$2"
}

# Sources under apps/, excluding vendored deps and test-internal executables.
app_srcs() {
    find apps -type f \( -name '*.c' -o -name '*.h' \) |
        grep -v '/deps/' |
        grep -vE '/(gui_selftest|client_parity_(manifest|replay)|test_[^/]*|tp_bench_[^/]*)\.(c|h)$'
}

# Shipping app/core sources only. Documentation, fixtures, unit tests, spikes,
# benchmarks, and the GUI selftest oracle are deliberately outside shipping
# dependency gates; tests may intentionally construct rejected input shapes.
shipping_srcs() {
    app_srcs
    find packer/include/tp_core packer/src -type f \
        \( -name '*.c' -o -name '*.h' \)
}

# 0. Corpus guard. Every apps/-scoped rule below (R1-R3, R8, R21, and
#    shipping_srcs' R15/R16a/R17/R18/R20) is only as strong as the file list it
#    scans, and an over-matching exclusion in app_srcs would drain that list and
#    turn all of them silently green. No per-regex self-test can see that: they
#    seed a violation into a printf, not into the real corpus. Assert the list is
#    non-empty and still names a file that must always be in scope.
_app_srcs=$(app_srcs)
if [ -z "$_app_srcs" ]; then
    hit "R0 app source corpus is empty" \
        "app_srcs matched no files -- every apps/-scoped rule below is vacuous"
else
    for _sentinel in apps/cli/main.c apps/gui/main.c; do
        if ! printf '%s\n' "$_app_srcs" | grep -qx -- "$_sentinel"; then
            hit "R0 app source corpus lost a sentinel file" \
                "$_sentinel is absent from app_srcs -- an exclusion is over-matching"
        fi
    done
fi

# 1. No sprite-name extension stripping outside tp_core (tp_sprite_export_key is
#    the single owner). Project-FILENAME helpers must carry a boundary-ok note.
_r1_extstrip="strrchr\([^,]+, *'\.'\)"
r1=$(app_srcs | xargs grep -nE "$_r1_extstrip" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r1" ] && hit "R1 ext-strip outside tp_core" "$r1"

# 2. No exporter-id string literals in frontends (use TP_EXPORTER_ID_* / registry).
_r2_exporter_id='"(json-neotolis|defold)"'
r2=$(app_srcs | xargs grep -nE "$_r2_exporter_id" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r2" ] && hit "R2 exporter-id literal in frontend" "$r2"

# 3. No pack-desc assembly in frontends (tp_pack_input_build owns encoding).
_r3_desc_assembly='ov_mask|TP_PACK_OV_'
r3=$(app_srcs | xargs grep -nE "$_r3_desc_assembly" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r3" ] && hit "R3 desc assembly in frontend" "$r3"

# Self-test: R1/R2/R3 are the three oldest detectors and were the only ones left
# without a seeded violation. Like every rule below, they now fail closed -- a
# future edit that breaks one of the regexes is caught here, not by a silently
# green tree -- and are pinned against false-positives on the legitimate
# registry/constant forms they exist to steer callers toward.
if ! printf "    const char *dot = strrchr(name, '.');\n" | grep -qE "$_r1_extstrip"; then
    hit "R1-selftest" "R1 detector failed to catch a seeded extension strip"
fi
if printf '    tp_sprite_export_key(name, key, sizeof key);\n' | grep -qE "$_r1_extstrip"; then
    hit "R1-selftest" "R1 detector false-positives on the tp_core export-key owner"
fi
for _seed in \
    '    if (strcmp(id, "json-neotolis") == 0) {' \
    '    const char *fallback = "defold";'
do
    if ! printf '%s\n' "$_seed" | grep -qE "$_r2_exporter_id"; then
        hit "R2-selftest" "R2 detector failed to catch a seeded exporter-id literal: $_seed"
    fi
done
if printf '    const char *id = TP_EXPORTER_ID_DEFOLD;\n    tp_exporter_at(i);\n' |
    grep -qE "$_r2_exporter_id"; then
    hit "R2-selftest" "R2 detector false-positives on the exporter-id constant / registry form"
fi
for _seed in \
    '    desc.ov_mask |= TP_PACK_OV_PADDING;' \
    '    settings.ov_mask = 0U;'
do
    if ! printf '%s\n' "$_seed" | grep -qE "$_r3_desc_assembly"; then
        hit "R3-selftest" "R3 detector failed to catch a seeded pack-desc assembly: $_seed"
    fi
done
if printf '    tp_pack_input_build(snapshot, atlas_id, &input, &err);\n' |
    grep -qE "$_r3_desc_assembly"; then
    hit "R3-selftest" "R3 detector false-positives on the tp_pack_input_build owner"
fi

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
_cutover=$(find apps/cli -maxdepth 1 -type f -name 'cli_mutate*.c')
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

if [ -n "$_cutover" ]; then
    r6a=$(printf '%s\n' "$_cutover" | xargs grep -nE "$_muts" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$r6a" ] && hit "R6a inline project mutator in cli_mutate (use a tp_operation)" "$r6a"
    r6b=$(printf '%s\n' "$_cutover" | xargs grep -nE "$_projwrite" 2>/dev/null | grep -v 'boundary-ok:')
    [ -n "$r6b" ] && hit "R6b direct write into the loaded project in cli_mutate (build an op)" "$r6b"
    r6c=$(printf '%s\n' "$_cutover" | xargs grep -nE "$_aliaswrite" 2>/dev/null | grep -v 'boundary-ok:')
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
#    main.c (app shell) are out of scope, as is the sanctioned
#    tp_project_atlas_seed_default_target lifecycle call. (H/P1-2 retired the former
#    animation-rename direct-write exception: animation rename
#    now routes through TP_OP_ANIMATION_RENAME.) A legit exception carries a "boundary-ok:" note
#    on the same line.
_gui_muts="apps/gui/gui_project.c apps/gui/gui_view_settings.c apps/gui/gui_view_lists.c apps/gui/gui_view_chrome.c apps/gui/gui_actions.c"
# The inline project mutators the F2-05b-i ops replaced (as R6,
# seed_default_target is lifecycle and not banned).
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
#     filesystem/lock backend, Pack, or Export implementation. The snapshot/query
#     Snapshot creation/query TUs are the same orchestration boundary and are
#     gated too.
_session_deps='#include[[:space:]]*[<"][^>"]*(apps/|gui|cli|protocol|cJSON)'
_session_impl='(^|[^A-Za-z0-9_])(fopen|fwrite|open|CreateFile|LockFile|tp_journal_(encode|decode)[A-Za-z0-9_]*|tp_pack|tp_export_run)[[:space:]]*\('
# All session TUs plus the shared private header. R10c's
# mutable-project-ownership scan runs over the .c TUs only.
_session_gate_srcs="packer/src/tp_session.c packer/src/tp_session_snapshot.c packer/src/tp_session_snapshot_query.c packer/src/tp_session_internal.h"
_session_gate_owner_srcs="packer/src/tp_session.c packer/src/tp_session_snapshot.c packer/src/tp_session_snapshot_query.c"
r10a=$(grep -nE "$_session_deps" $_session_gate_srcs 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r10a" ] && hit "R10a frontend/protocol dependency in tp_session" "$r10a"
r10b=$(grep -nE "$_session_impl" $_session_gate_srcs 2>/dev/null |
    grep -v 'boundary-ok:')
[ -n "$r10b" ] && hit "R10b backend/codec/Pack/Export implementation in tp_session" "$r10b"
_session_model_owner='->[[:space:]]*project([^A-Za-z0-9_]|$)'
r10c=$(sed -E 's/snapshot->[[:space:]]*project([^A-Za-z0-9_]|$)/\1/g' \
    $_session_gate_owner_srcs 2>/dev/null |
    grep -nE -- "$_session_model_owner")
[ -n "$r10c" ] && hit "R10c mutable-project ownership in tp_session" "$r10c"
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
if ! printf '    tp_model *model = session->model;\n    model->project = replacement;\n' |
    grep -qE -- "$_session_model_owner"; then
    hit "R10-selftest" "R10c detector failed to catch seeded mutable-model ownership"
fi
if printf '    snapshot->project = cloned;\n' |
    sed -E 's/snapshot->[[:space:]]*project([^A-Za-z0-9_]|$)/\1/g' |
    grep -qE -- "$_session_model_owner"; then
    hit "R10-selftest" "R10c detector false-positives on snapshot ownership"
fi
if ! printf '    snapshot->project = model->project;\n' |
    sed -E 's/snapshot->[[:space:]]*project([^A-Za-z0-9_]|$)/\1/g' |
    grep -qE -- "$_session_model_owner"; then
    hit "R10-selftest" "R10c detector missed mixed snapshot/model ownership"
fi

# 11. Source filesystem truth is a session-owned projection. The deleted GUI
#     scan/cache path must not reappear in shipping code or tests.
r11=$(find apps/gui -type f \( -name '*.c' -o -name '*.h' \) |
    xargs grep -nE 'gui_scan(_[A-Za-z0-9_]+)?[[:space:]]*\(' 2>/dev/null)
[ -n "$r11" ] && hit "R11 legacy GUI source scan/cache path" "$r11"
if ! printf '    gui_scan_get(path, &result, &error);\n' |
    grep -qE 'gui_scan(_[A-Za-z0-9_]+)?[[:space:]]*\('; then
    hit "R11-selftest" "R11 detector failed to catch a seeded GUI scan"
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
#     not a sanctioned second route. Scoped to all three gui_pack orchestration
#     TUs -- gui_pack.c (result/ref-index storage), gui_pack_jobs.c (the
#     session-job start/poll/cancel adapter that replaced the old worker
#     thread), and gui_pack_preview.c (preview slot + loss-diff cache) -- so a
#     thread/atomic/tp_pack authority regression re-introduced into either
#     split-out file is caught, not only a regression in gui_pack.c.
_gui_job_srcs="apps/gui/gui_pack.c apps/gui/gui_pack_jobs.c apps/gui/gui_pack_preview.c"
_gui_job_owner='(^|[^A-Za-z0-9_])(thrd_(create|join)|atomic_[A-Za-z0-9_]*|_Atomic|pack_worker|export_worker|s_job(_active)?|tp_pack[[:space:]]*\(|tp_pack_input_build[A-Za-z0-9_]*[[:space:]]*\(|tp_pack_settings_build[A-Za-z0-9_]*[[:space:]]*\(|tp_export_snapshot_job_[A-Za-z0-9_]*[[:space:]]*\(|tp_pack_sprite_desc)([^A-Za-z0-9_]|$)'
r13=$(grep -nE "$_gui_job_owner" $_gui_job_srcs 2>/dev/null |
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

# Self-test: prove the file-scope extension itself is intact -- $_gui_job_srcs
# (the exact argument list r13 greps) must still name both split-out TUs, so a
# future edit that quietly shrinks the list back to gui_pack.c alone is caught
# here rather than silently losing coverage of the split files.
case " $_gui_job_srcs " in
    *' apps/gui/gui_pack_jobs.c '*) ;;
    *) hit "R13-selftest" "R13 file scope no longer includes apps/gui/gui_pack_jobs.c" ;;
esac
case " $_gui_job_srcs " in
    *' apps/gui/gui_pack_preview.c '*) ;;
    *) hit "R13-selftest" "R13 file scope no longer includes apps/gui/gui_pack_preview.c" ;;
esac

# Self-test: prove the detector, invoked exactly as r13 invokes it (grep across
# the full $_gui_job_srcs argument list plus one extra file), still fires on a
# violation seeded into a scratch file named after a split-out TU and still
# ignores the legitimate session-job API form in that same multi-file scan.
_r13_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r13_dir" ] || [ ! -d "$_r13_dir" ]; then
    hit "R13-selftest" "R13 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r13_dir"' EXIT
    printf '    thrd_create(&s_job.thread, pack_worker, ctx);\n' >"$_r13_dir/gui_pack_jobs.c"
    r13_scope=$(grep -nE "$_gui_job_owner" $_gui_job_srcs "$_r13_dir/gui_pack_jobs.c" 2>/dev/null |
        grep -v 'boundary-ok:')
    if ! printf '%s\n' "$r13_scope" | grep -q "$_r13_dir/gui_pack_jobs.c"; then
        hit "R13-selftest" "R13 detector failed to catch a seeded violation in a split-out-TU scan"
    fi
    printf '    tp_session_pack_job_start(session, &request, &error);\n' >"$_r13_dir/gui_pack_preview.c"
    r13_scope2=$(grep -nE "$_gui_job_owner" "$_r13_dir/gui_pack_preview.c" 2>/dev/null |
        grep -v 'boundary-ok:')
    if [ -n "$r13_scope2" ]; then
        hit "R13-selftest" "R13 detector false-positives on typed job orchestration in a split-out-TU scan"
    fi
    rm -rf "$_r13_dir"
    trap - EXIT
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

# 17. Comment hygiene: shipping source comments are short WHY only, never a phase/
#     review tag. Bans the bracketed fix/review markers, R5b-x phase labels, Fx-xx
#     phase tags (incl. suffixed F2-05b-ii-A forms), and the Dx: crash-diagnostic
#     comment prefix. Durable contract references are deliberately NOT matched and
#     must survive. The Fx-xx alternative omits a trailing \b so a reintroduced F2-05a /
#     F2-05b-i variant is still caught; the Dx: alternative excludes a leading letter
#     or '%' so "%H:%M:%S" strftime and "PATH:" are not flagged. Test/bench/selftest
#     sources are outside shipping_srcs and keep their oracles. A legit hit (e.g. a
#     string literal) may carry a "boundary-ok:" note on the same line.
_comment_tags='\b(fix|review) \[[0-9]+\]|\bR5b-[0-9]|\bF[12]-[0-9]{2}|(^|[^%A-Za-z])D[12]:'
r17=$(shipping_srcs | xargs grep -nE "$_comment_tags" 2>/dev/null | grep -v 'boundary-ok:')
[ -n "$r17" ] && hit "R17 phase/review/diagnostic tag in shipping source comment" "$r17"

# Self-test: the detector FIRES on each seeded tag form and does NOT false-positive on
# the permitted suffix/strftime/PATH/section-reference forms (asserted every run).
for _seed in \
    '#include "x.h" /* fix [3] */' \
    '/* R5b-2 read-only opener */' \
    '/* F2-03 task 1: capture */' \
    '/* F2-05b-ii-A gesture coalescing */' \
    '#include "gui_crash.h" /* D2: crash handler */' \
    '/* D1: app-data root */'
do
    if ! printf '%s\n' "$_seed" | grep -qE "$_comment_tags"; then
        hit "R17-selftest" "R17 detector failed to catch a seeded tag: $_seed"
    fi
done
if printf '%s\n' \
    '/* keep the derived suffix [0] slot */' \
    '    (void)snprintf(t, sizeof t, "%H:%M:%S", tm);' \
    '    const char *k = "PATH:";' \
    '/* selector resolution (model contract) */' \
    '/* dedup pending; never merge */' \
    '/* order rule: id-keyed collections */' |
    grep -qE "$_comment_tags"; then
    hit "R17-selftest" "R17 detector false-positives on legitimate suffix/strftime/PATH/section-reference content"
fi

# 18. Internal-header discipline. A *_internal.h (or the tp_model_seam.h /
#     tp_recovery_live_seam.h / tp_session_layout.h lifecycle/layout seams) is a
#     component-private contract; only the TUs registered for it -- the owning
#     family plus any explicitly allowlisted seam consumer -- may include it.
#     Keeps a private contract from leaking into a module it was never designed to
#     couple with. Scoped to shipping_srcs (test/bench/selftest sources are outside
#     it and keep wider access, as with every other rule in this script). An include
#     guarded by `#ifdef NTPACKER_GUI_SELFTEST` is dev-seam wiring compiled out of the
#     shipped binary, not a production coupling, so the scanner skips it; a legit hit
#     may still carry a "boundary-ok:" note on the same line.
#
# Registry: header name -> its allowed includer basenames (family + allowlisted
# seams), one row per internal header that currently exists. tp_txn_parse_priv.h
# also lives in packer/src but does not match the *_internal.h shape this rule scans
# for, so it is deliberately left out of the registry and the scan.
_internal_header_registry() {
    cat <<'EOF'
cli_mutate_internal     cli_mutate|cli_mutate_source|cli_mutate_atlas|cli_mutate_sprite|cli_mutate_anim|cli_mutate_target
gui_actions_internal    gui_actions|gui_actions_dialogs|gui_actions_edits|gui_actions_intents|gui_actions_pack|gui_actions_preview|gui_actions_recovery
gui_canvas_internal     gui_canvas|gui_canvas_resources
gui_pack_internal       gui_pack|gui_pack_jobs|gui_pack_preview
gui_project_internal    gui_project|gui_project_file|gui_project_mutations|gui_project_pending|gui_project_recovery
tp_diff_internal        tp_diff_entity|tp_diff_apply|tp_diff_capture|tp_history|tp_history_codec|tp_history_codec_internal|tp_history_codec_read|tp_model|tp_model_journal|tp_op_apply|tp_txn_apply
tp_op_internal          tp_op_catalog|tp_op_validate|tp_op_validate_atlas|tp_op_validate_source_sprite|tp_op_validate_animation|tp_op_validate_target|tp_op_apply|tp_op_build|tp_op_encode|tp_model_journal|tp_txn_encode|tp_txn_apply|tp_txn_lower|tp_txn_parse|tp_txn_result
tp_op_validate_family_internal tp_op_validate|tp_op_validate_atlas|tp_op_validate_source_sprite|tp_op_validate_animation|tp_op_validate_target
tp_encode_internal      tp_op_encode|tp_txn_encode|tp_txn_apply
tp_fs_internal          tp_fs_io|tp_fs_win32|tp_fs_posix|tp_build_worker|tp_job_worker_main|tp_job|tp_export_defold|tp_export_json_neotolis|tp_export_png|tp_file_lease|tp_identity|tp_image|tp_journal_io|tp_pack_hash|tp_pack_read|tp_project_parse|tp_project_save|tp_project_lease|tp_recovery|tp_recovery_backend_win32|tp_recovery_scan|tp_recovery_store|tp_scan|tp_format_discovery_win32|tp_runtime_path_win32
tp_format_catalog_internal tp_format_catalog|tp_format_catalog_scan|tp_export_run
tp_format_descriptor_internal tp_format_catalog_internal|tp_format_descriptor|tp_format_discovery_posix|tp_format_discovery_win32
tp_format_diagnostic_internal tp_format_catalog_scan|tp_format_diagnostic
tp_format_discovery_internal tp_format_catalog_scan|tp_format_discovery|tp_format_discovery_posix|tp_format_discovery_win32
tp_pack_constraints_internal tp_pack_constraints|tp_op_validate_atlas|tp_op_validate_source_sprite|tp_pack|tp_project_identity|tp_project_parse|tp_validate_target_settings
tp_history_codec_internal tp_history|tp_history_codec|tp_history_codec_read|tp_model_journal|tp_txn_apply
tp_journal_internal     tp_journal|tp_journal_io|tp_journal_read|tp_journal_wire|tp_history|tp_model|tp_model_journal|tp_txn_apply|tp_recovery_backend_posix|tp_recovery_backend_win32|tp_recovery_claim|tp_recovery_scan|tp_recovery_store
tp_json_internal        tp_json_internal|tp_project_parse|tp_txn_parse|tp_format_descriptor
tp_utf8_internal        tp_utf8|tp_build_proto|tp_fs_io|tp_image|tp_job_worker_proto|tp_json_internal|tp_op_validate|tp_project_identity|tp_source_path_text|tp_format_catalog_scan|tp_format_descriptor
tp_idset_internal       tp_idset|tp_txn_idset|tp_journal|tp_journal_internal|tp_journal_read|tp_model_journal
tp_project_internal     tp_project|tp_project_identity|tp_project_parse|tp_project_parse_internal|tp_project_save|tp_project_write_internal|tp_project_write|tp_history|tp_job|tp_model_journal|tp_session
tp_project_path_internal tp_project|tp_project_parse|tp_project_path|tp_project_save|tp_project_write
tp_project_model_internal tp_project|tp_project_parse
tp_project_parse_internal tp_project_parse|tp_project_write
tp_project_write_internal tp_project_parse|tp_project_write
tp_project_identity_internal tp_project_identity|tp_project_parse|tp_project_write|tp_history_codec_read|tp_input|tp_model|tp_model_journal|tp_op_validate_atlas|tp_op_validate_source_sprite|tp_op_validate_animation|tp_op_validate_target|tp_session|tp_txn_apply
tp_project_generation_internal tp_model|tp_project_generation|tp_session_snapshot
tp_project_mutation_internal tp_project|tp_project_parse|tp_diff_entity|tp_diff_apply|tp_diff_capture|tp_export_run|tp_input|tp_op_apply|tp_op_validate|tp_op_validate_animation|tp_op_validate_target|tp_session|tp_session_snapshot|tp_session_snapshot_query
tp_txn_internal         tp_model|tp_model_journal|tp_txn_apply|tp_txn_parse|tp_txn_encode|tp_txn_idset|tp_txn_lower|tp_txn_result|tp_project_clone|tp_history
tp_model_seam           tp_session|tp_session_snapshot|tp_recovery_claim|tp_recovery_store|tp_txn_internal|tp_txn_apply|tp_txn_parse|tp_txn_encode|tp_txn_idset|tp_txn_lower|tp_project_clone|tp_history
tp_recovery_live_seam   tp_session|tp_recovery|tp_recovery_internal
tp_session_internal     tp_session|tp_session_snapshot|tp_session_layout|tp_recovery|tp_recovery_claim|tp_validate|tp_validate_target_settings|tp_export|tp_export_run|tp_input|tp_sprite_index
tp_session_layout       tp_session|tp_session_snapshot
tp_session_snapshot_internal tp_job|tp_session|tp_session_snapshot|tp_session_snapshot_query
tp_recovery_backend_types_internal tp_recovery_backend_posix|tp_recovery_backend_win32|tp_recovery_state_internal
tp_recovery_internal    tp_recovery|tp_recovery_state_internal|tp_recovery_claim|tp_recovery_scan|tp_recovery_store
tp_recovery_state_internal tp_recovery|tp_recovery_claim|tp_recovery_scan|tp_recovery_store
tp_job_owner_internal   tp_session|tp_job|tp_refresh_job
tp_source_runtime_internal tp_source_runtime|tp_refresh_job|tp_session
tp_project_owned_internal tp_project_owned|tp_project|tp_project_clone|tp_diff_entity
tp_source_plan_internal tp_source_plan|tp_op_validate|tp_op_validate_source_sprite
tp_source_path_text_internal tp_source_path_text|tp_op_validate|tp_project|tp_project_identity|tp_project_parse|tp_source_plan
tp_srckey_internal      tp_srckey|tp_project_identity|tp_op_validate_animation|tp_validate_source|tp_validate_sprite
tp_validate_internal    tp_validate|tp_validate_index|tp_validate_report|tp_validate_sprite
tp_validate_index_internal tp_validate|tp_validate_index|tp_validate_rules_internal|tp_validate_source|tp_validate_sprite
tp_validate_report_internal tp_validate|tp_validate_report|tp_validate_rules_internal|tp_validate_source|tp_validate_sprite
tp_validate_rules_internal tp_validate|tp_validate_source|tp_validate_sprite|tp_validate_target_settings
tp_identity_internal    tp_identity|tp_identity_session
tp_pack_read_internal   tp_pack_read
tp_build_driver_internal tp_build_driver|tp_build_worker|tp_build_worker_main
tp_build_proto_internal tp_build_proto|tp_build_worker|tp_build_worker_main
tp_build_worker_internal tp_build_worker|tp_pack|tp_job_worker_main
tp_job_worker_internal  tp_build_worker_main|tp_job_worker_main|tp_job_worker_process_internal|tp_job_worker_proto
tp_job_worker_process_internal tp_job|tp_job_worker
tp_export_internal     tp_export|tp_export_defold|tp_export_json_neotolis|tp_export_png|tp_export_run|tp_format_catalog
tp_export_job_internal tp_export_run|tp_job_worker_main
tp_proc_internal        tp_proc_win32|tp_proc_posix|tp_build_worker|tp_job_worker|tp_job_worker_main
EOF
}

# Scanner: for each file given, emits "includer header path:line" per internal-header
# include, skipping lines inside an `#ifdef NTPACKER_GUI_SELFTEST` guard (tracked with
# a depth counter so a guard that nests an unrelated #if/#endif is still closed by its
# own #endif) and lines carrying a "boundary-ok:" note.
_internal_header_scan() {
    awk '
        FNR == 1 { guard = 0 }
        guard == 0 && /^[[:space:]]*#[[:space:]]*ifdef[[:space:]]+NTPACKER_GUI_SELFTEST([[:space:]]|$)/ {
            guard = 1
            next
        }
        guard > 0 && /^[[:space:]]*#[[:space:]]*(ifdef|ifndef|if)([[:space:]]|\()/ {
            guard++
            next
        }
        guard > 0 && /^[[:space:]]*#[[:space:]]*endif/ {
            guard--
            next
        }
        guard > 0 { next }
        (/#include[[:space:]]*"[A-Za-z0-9_]+_internal\.h"/ || /#include[[:space:]]*"tp_model_seam\.h"/ || /#include[[:space:]]*"tp_recovery_live_seam\.h"/ || /#include[[:space:]]*"tp_session_layout\.h"/) {
            if ($0 ~ /boundary-ok:/) next
            line = $0
            if (match(line, /"[A-Za-z0-9_]+\.h"/)) {
                header = substr(line, RSTART + 1, RLENGTH - 2)
                sub(/\.h$/, "", header)
                n = split(FILENAME, parts, "/")
                includer = parts[n]
                sub(/\.[ch]$/, "", includer)
                printf "%s %s %s:%d\n", includer, header, FILENAME, FNR
            }
        }
    ' "$@" 2>/dev/null
}

# Checker: reads "includer header path:line" rows from stdin and fails on (a) an
# includer not registered for its header's allowed list, or (b) a header the scan
# found that has no registry row at all.
_internal_header_check() {
    local registry="$1" includer header pathline allowed
    while read -r includer header pathline; do
        [ -z "$includer" ] && continue
        allowed=$(printf '%s\n' "$registry" | awk -v h="$header" '$1 == h { print $2 }')
        if [ -z "$allowed" ]; then
            printf 'R18 %s not registered in the internal-header registry (include at %s)\n' "$header" "$pathline"
            continue
        fi
        case "|$allowed|" in
            *"|$includer|"*) ;;
            *)
                printf 'R18 %s may not include %s (allowed includers: %s) at %s\n' \
                    "$includer" "$header" "$allowed" "$pathline"
                ;;
        esac
    done
}

r18=$(_internal_header_scan $(shipping_srcs) | _internal_header_check "$(_internal_header_registry)")
[ -n "$r18" ] && hit "R18 internal-header discipline" "$r18"

# Self-test: seeded fixtures under a scratch dir prove the scanner/checker pair
# actually fires on a cross-module include and an unregistered header, stays quiet
# for a registered family/seam includer, and that the guard skip is a scanner-level
# behavior (not just a checker false-negative) -- asserted every run.
_r18_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r18_dir" ] || [ ! -d "$_r18_dir" ]; then
    hit "R18-selftest" "R18 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r18_dir"' EXIT

    printf '#include "tp_txn_internal.h"\n' >"$_r18_dir/fake_frontend.c"
    printf '#include "tp_totally_unregistered_internal.h"\n' >"$_r18_dir/fake_unregistered.c"
    printf '#include "tp_txn_internal.h"\n' >"$_r18_dir/tp_txn_apply.c"
    printf '#ifdef NTPACKER_GUI_SELFTEST\n#include "tp_txn_internal.h"\n#endif\n' >"$_r18_dir/fake_guarded.c"

    _r18_registry=$(_internal_header_registry)

    _r18_scan1=$(_internal_header_scan "$_r18_dir/fake_frontend.c")
    _r18_check1=$(printf '%s\n' "$_r18_scan1" | _internal_header_check "$_r18_registry")
    if [ -z "$_r18_check1" ]; then
        hit "R18-selftest" "R18 checker failed to catch a seeded cross-module internal-header include"
    fi

    _r18_scan2=$(_internal_header_scan "$_r18_dir/fake_unregistered.c")
    _r18_check2=$(printf '%s\n' "$_r18_scan2" | _internal_header_check "$_r18_registry")
    if [ -z "$_r18_check2" ]; then
        hit "R18-selftest" "R18 checker failed to catch an include of a header missing from the registry"
    fi

    _r18_scan3=$(_internal_header_scan "$_r18_dir/tp_txn_apply.c")
    _r18_check3=$(printf '%s\n' "$_r18_scan3" | _internal_header_check "$_r18_registry")
    if [ -n "$_r18_check3" ]; then
        hit "R18-selftest" "R18 checker false-positives on a registered family member's include"
    fi

    _r18_scan4=$(_internal_header_scan "$_r18_dir/fake_guarded.c")
    if [ -n "$_r18_scan4" ]; then
        hit "R18-selftest" "R18 scanner failed to skip an NTPACKER_GUI_SELFTEST-guarded include"
    fi

    _r18_scan5=$(_internal_header_scan "$_r18_dir/fake_frontend.c")
    if [ -z "$_r18_scan5" ]; then
        hit "R18-selftest" "R18 scanner failed to emit an unguarded internal-header include"
    fi

    rm -rf "$_r18_dir"
    trap - EXIT
fi

# 19. The live mutable project borrowed from tp_model is a core-only seam. Public
#     clients mutate through tp_session and read through immutable snapshots; putting
#     tp_model_project back under packer/include would reopen the raw-authority escape.
_public_model_project='(^|[^A-Za-z0-9_])tp_model_project[[:space:]]*\('
r19=$(for f in packer/include/tp_core/*.h; do
    sed -e 's|/\*.*\*/||g' -e 's|//.*||' -e 's|/\*.*||' -e '/^[[:space:]]*\*/d' "$f" |
        grep -nE "$_public_model_project" | sed "s|^|$f:|"
done)
[ -n "$r19" ] && hit "R19 mutable model project escape in public tp header" "$r19"

if ! printf 'tp_project *tp_model_project(tp_model *model);\n' |
    grep -qE "$_public_model_project"; then
    hit "R19-selftest" "R19 detector failed to catch a seeded public tp_model_project declaration"
fi
if printf 'tp_project *tp_model_project_view(tp_model *model);\n' |
    grep -qE "$_public_model_project"; then
    hit "R19-selftest" "R19 detector false-positives on a distinct symbol"
fi

# 20. The model project seam is an immutable borrowed view. Production code may
#     clone that view for a private candidate, but may not bind it as mutable or
#     cast const away. The C type system catches ordinary direct calls to mutable
#     APIs; this source gate pins the seam declaration and blocks explicit escape
#     hatches before they become an ownership convention.
_mutable_model_project_scan() {
    awk '
        {
            line = $0
            sub(/\/\/.*/, "", line)
            if (line !~ /tp_model_project[[:space:]]*\(/) next
            mutable_binding = line ~ /tp_project[[:space:]]*\*[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*tp_model_project[[:space:]]*\(/
            const_binding = line ~ /const[[:space:]]+tp_project[[:space:]]*\*[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*tp_model_project[[:space:]]*\(/
            cast_away_const = line ~ /\([[:space:]]*tp_project[[:space:]]*\*[[:space:]]*\)[[:space:]]*tp_model_project[[:space:]]*\(/
            if ((mutable_binding && !const_binding) || cast_away_const) {
                printf "%s:%d:%s\n", FILENAME, FNR, $0
            }
        }
    ' "$@" 2>/dev/null
}

_const_model_project_decl='^[[:space:]]*const[[:space:]]+tp_project[[:space:]]*\*[[:space:]]*tp_model_project[[:space:]]*\([[:space:]]*const[[:space:]]+tp_model[[:space:]]*\*[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\)[[:space:]]*;'
if ! grep -qE "$_const_model_project_decl" packer/src/tp_model_seam.h; then
    hit "R20 const model project seam" "tp_model_seam.h must expose tp_model_project as a const project view of a const model"
fi

r20=$(_mutable_model_project_scan $(shipping_srcs))
[ -n "$r20" ] && hit "R20 mutable model project consumer" "$r20"

_r20_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r20_dir" ] || [ ! -d "$_r20_dir" ]; then
    hit "R20-selftest" "R20 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r20_dir"' EXIT
    printf 'tp_project *p = tp_model_project(model);\n' >"$_r20_dir/mutable.c"
    printf 'tp_project *p = (tp_project *)tp_model_project(model);\n' >"$_r20_dir/cast.c"
    printf 'const tp_project *p = tp_model_project(model);\n' >"$_r20_dir/const.c"
    printf 'tp_project *p = tp_project_clone(tp_model_project(model));\n' >"$_r20_dir/clone.c"
    [ -z "$(_mutable_model_project_scan "$_r20_dir/mutable.c")" ] &&
        hit "R20-selftest" "R20 failed to catch a mutable model-project binding"
    [ -z "$(_mutable_model_project_scan "$_r20_dir/cast.c")" ] &&
        hit "R20-selftest" "R20 failed to catch a const-removing model-project cast"
    [ -n "$(_mutable_model_project_scan "$_r20_dir/const.c")" ] &&
        hit "R20-selftest" "R20 false-positives on a const model-project binding"
    [ -n "$(_mutable_model_project_scan "$_r20_dir/clone.c")" ] &&
        hit "R20-selftest" "R20 false-positives on a private candidate clone"
    rm -rf "$_r20_dir"
    trap - EXIT
fi

# 21. The strict UTF-8/long-path policy is one tp_core boundary. Frontends may
#     retain CRT-local fopen/remove/rename adapters, but must not reimplement
#     decoding, absolute-path resolution, namespace policy, or Win32 error maps.
#     nt_utf8_argv.c is the explicit process-ingress exception: Windows supplies
#     UTF-16 argv, so that boundary legitimately encodes it once as UTF-8.
_frontend_fs_policy='(MultiByteToWideChar|WideCharToMultiByte|GetFullPathNameW|win32_error_to_errno|ERROR_FILENAME_EXCED_RANGE|UNC\\\\)'
_frontend_fs_ingress='apps/common/nt_utf8_argv.c'
_frontend_fs_policy_scan() {
    grep -nE "$_frontend_fs_policy" "$@" 2>/dev/null |
        grep -v 'boundary-ok:'
}
r21=$(_frontend_fs_policy_scan $(app_srcs | grep -v "^$_frontend_fs_ingress$"))
[ -n "$r21" ] && hit "R21 duplicate frontend filesystem policy" "$r21"

_r21_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r21_dir" ] || [ ! -d "$_r21_dir" ]; then
    hit "R21-selftest" "R21 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r21_dir"' EXIT
    mkdir -p "$_r21_dir/apps/gui" "$_r21_dir/apps/cli"
    printf '    GetFullPathNameW(path, 0, NULL, NULL);\n' >"$_r21_dir/apps/gui/seeded_path_policy.c"
    printf '    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, cap);\n' >"$_r21_dir/apps/cli/seeded_decode_policy.c"
    [ -z "$(_frontend_fs_policy_scan "$_r21_dir/apps/gui/seeded_path_policy.c")" ] &&
        hit "R21-selftest" "R21 failed to catch seeded GUI path policy"
    [ -z "$(_frontend_fs_policy_scan "$_r21_dir/apps/cli/seeded_decode_policy.c")" ] &&
        hit "R21-selftest" "R21 failed to catch seeded CLI decode policy"
    rm -rf "$_r21_dir"
    trap - EXIT
fi

# 22. Retained verification-id traceability. The pre-session-view USA ids were
#     removed with their obsolete observation/client contracts; every remaining
#     USA id must still be OWNED,
#     and ownership has exactly two classes:
#       test -- a `/* USA-nn ... */` tag within 3 lines of the `void test_`
#               definition it claims, in a TEST source file, AND that function
#               must be invoked somewhere in the same file (RUN_TEST(name); --
#               same-line or with the name wrapped to the next line -- or a bare
#               name();). Test functions are non-static, so deleting only the
#               RUN_TEST line compiles warning-free; without the call-site
#               requirement the tag would keep crediting a test that no longer
#               runs;
#       gate -- a registry line in a build check (cmake/, scripts/) carrying both
#               the id and the literal marker `owner: gate`, for the handful of
#               requirements whose proof IS a build gate and not a runtime test.
#     Partial coverage is recorded with a "partial:" note -- in the test tag, or
#     on the gate registry line for a gate-class owner -- and still counts as
#     owned: the gate proves a requirement has not silently lost its last owner,
#     not that the owner is exhaustive. A gate owner whose rule carries
#     per-symbol debt allowances is partial by construction and says so.
#
#     The corpus used to be a bare `grep -r USA- apps packer/tests`, which any
#     production comment, CMakeLists note, or the gate's own self-test text
#     satisfied: an id could keep its tag long after its last test was deleted.
_usa_ids() {
    printf '%s\n' \
        USA-11 USA-14 USA-15 USA-16 USA-17 USA-18 USA-19 USA-20 USA-21 \
        USA-22 USA-23 USA-24 USA-25 USA-27 USA-28 USA-29 USA-31
}
# Reads the ids present in a corpus on $1, prints the ids that are absent.
_usa_missing() {
    _usa_present="$1"
    _usa_ids | while read -r _usa_id; do
        printf '%s\n' "$_usa_present" | grep -qx -- "$_usa_id" ||
            printf '%s\n' "$_usa_id"
    done
}
# Both app test-file spellings are corpus: apps/gui uses `test_*.c`, apps/cli
# uses `*_test.c` (cli_out_contract_test.c), so a tag placed under the CLI
# convention is visible to this gate instead of silently unowned.
_usa_test_sources() {
    ls apps/*/test_*.c apps/*/*_test.c apps/gui/gui_selftest.c packer/tests/*.c \
        2>/dev/null
}
# Ids claimed by a tag adjacent to the `void test_` definition it owns. A tag
# with no test under it (deleted owner) contributes nothing, and neither does a
# definition that is never invoked in its own file: the id is credited only
# when the file also carries a call site for the claimed function -- the
# trailing `name);` of a RUN_TEST(name); (same-line or wrapped) or a bare
# direct call `name();`. Prototypes (`void name(void);`) match neither form.
#
# The call scan is textual, so a call site that the COMPILER never sees must be
# removed before it is recorded, or the tag keeps crediting a test that no
# longer runs. Two such forms are stripped here: text inside a /* ... */ block
# comment (and after a // line comment) but NOT inside a string or character
# literal, and text inside an `#if 0 ... #endif` region (nesting tracked; the
# `#else` branch of an `#if 0` is live code and is NOT stripped). Only the
# literal `#if 0` / `#elif 0` conditions are treated as dead -- this is a
# greppable gate, not a preprocessor, so no other condition is evaluated.
# Tag scanning deliberately reads the RAW line, because the `/* USA-nn */` tags
# themselves live in comments; only `#if 0` regions drop out of both scans.
#
# Known limits (documented rather than detected -- each needs analysis this gate
# does not do):
#   - dead call graph: a `RUN_TEST(test_foo);` inside a function that main()
#     never calls still counts. Proving otherwise needs whole-file call-graph
#     analysis, and a runner file's entry point is not a fixed name here;
#   - conditional compilation other than `#if 0` (e.g. a registration under
#     `#ifdef _WIN32`) counts on every platform;
#   - a call site in a DIFFERENT file than the definition is not seen, so a
#     shared runner would read as unregistered.
_usa_test_owned() {
    awk '
        function flush_file(   fn, n, i, part) {
            for (fn in claim)
                if (fn in called) {
                    n = split(claim[fn], part, " ")
                    for (i = 1; i <= n; i++)
                        if (part[i] != "") print part[i]
                }
            split("", claim); split("", called)
        }
        # Removes commented-out text, carrying /* ... */ state across lines.
        # String and character literals are TEXT, not comment openers: a "%s/*"
        # glob and a "//server/share" UNC path are both live in this corpus, and
        # reading either as a comment start swallows the live code that follows
        # it -- including #else/#endif directives, which desyncs the dead-region
        # depth counters below. A literal never spans a line here, so literal
        # state resets per line; the block-comment state deliberately does not.
        # ("\047" is the apostrophe: this awk program is shell-single-quoted, so
        # a literal one cannot appear in it.)
        function strip_comments(line,   out, i, n, c, c2, in_str, in_chr) {
            if (!in_comment && index(line, "/") == 0 &&
                index(line, "\"") == 0 && index(line, "\047") == 0)
                return line
            out = ""
            in_str = 0
            in_chr = 0
            n = length(line)
            i = 1
            while (i <= n) {
                c = substr(line, i, 1)
                c2 = substr(line, i, 2)
                if (in_comment) {
                    if (c2 == "*/") { in_comment = 0; i += 2 } else { i++ }
                    continue
                }
                if (in_str || in_chr) {
                    out = out c
                    if (c == "\\") {
                        out = out substr(line, i + 1, 1)
                        i += 2
                        continue
                    }
                    if (in_str && c == "\"") in_str = 0
                    else if (in_chr && c == "\047") in_chr = 0
                    i++
                    continue
                }
                if (c2 == "/*") { in_comment = 1; i += 2; continue }
                if (c2 == "//") break
                out = out c
                if (c == "\"") in_str = 1
                else if (c == "\047") in_chr = 1
                i++
            }
            return out
        }
        # Tracks `#if 0` regions on already-comment-stripped text. Returns 1 for
        # a preprocessor directive line (never a call site itself).
        function track_dead_regions(line,   t) {
            t = line
            sub(/^[ \t]*/, "", t)
            if (t !~ /^#/) return 0
            if (t ~ /^#[ \t]*(if|ifdef|ifndef)([^A-Za-z0-9_]|$)/) {
                cond_depth++
                if (dead_depth == 0 && t ~ /^#[ \t]*if[ \t]+0[ \t]*$/)
                    dead_depth = cond_depth
                return 1
            }
            if (t ~ /^#[ \t]*endif([^A-Za-z0-9_]|$)/) {
                if (dead_depth == cond_depth) dead_depth = 0
                if (cond_depth > 0) cond_depth--
                return 1
            }
            if (t ~ /^#[ \t]*(else|elif)([^A-Za-z0-9_]|$)/) {
                if (dead_depth == cond_depth) dead_depth = 0
                # A literal `#elif 0` opens a dead branch exactly like `#if 0`;
                # every other #elif condition stays live (this is a greppable
                # gate, not a preprocessor).
                if (dead_depth == 0 && t ~ /^#[ \t]*elif[ \t]+0[ \t]*$/)
                    dead_depth = cond_depth
                return 1
            }
            return 0
        }
        function record_calls(line,   s, m) {
            s = line
            while (match(s, /test_[A-Za-z0-9_]*[ \t]*\)[ \t]*;/)) {
                if (RSTART == 1 || substr(s, RSTART - 1, 1) !~ /[A-Za-z0-9_]/) {
                    m = substr(s, RSTART, RLENGTH)
                    sub(/[ \t]*\).*/, "", m)
                    called[m] = 1
                }
                s = substr(s, RSTART + RLENGTH)
            }
            s = line
            while (match(s, /test_[A-Za-z0-9_]*[ \t]*\([ \t]*\)[ \t]*;/)) {
                if (RSTART == 1 || substr(s, RSTART - 1, 1) !~ /[A-Za-z0-9_]/) {
                    m = substr(s, RSTART, RLENGTH)
                    sub(/[ \t]*\(.*/, "", m)
                    called[m] = 1
                }
                s = substr(s, RSTART + RLENGTH)
            }
        }
        FNR == 1 {
            flush_file()
            p1 = ""; p2 = ""; p3 = ""
            in_comment = 0; cond_depth = 0; dead_depth = 0
        }
        {
            code = strip_comments($0)
            directive = track_dead_regions(code)
            if (dead_depth > 0 || directive) { p3 = p2; p2 = p1; p1 = $0; next }
            record_calls(code)
            if (match($0, /(^|[^A-Za-z0-9_])void[ \t]+test_[A-Za-z0-9_]*[ \t]*\(/)) {
                d = substr($0, RSTART, RLENGTH)
                sub(/.*void[ \t]+/, "", d)
                sub(/[ \t]*\(/, "", d)
                s = p3 " " p2 " " p1 " " $0
                while (match(s, /USA-[0-9][0-9]/)) {
                    claim[d] = claim[d] " " substr(s, RSTART, RLENGTH)
                    s = substr(s, RSTART + RLENGTH)
                }
            }
            p3 = p2; p2 = p1; p1 = $0
        }
        END { flush_file() }
    ' "$@" 2>/dev/null | sort -u
}
# Ids explicitly registered as build-gate-owned.
_usa_gate_owned() {
    grep -hoE 'USA-[0-9]{2}.*owner: gate' "$@" 2>/dev/null |
        grep -oE 'USA-[0-9]{2}' | sort -u
}
_usa_found=$( { _usa_test_owned $(_usa_test_sources)
                _usa_gate_owned cmake/*.cmake scripts/*.sh; } | sort -u)
r22=$(_usa_missing "$_usa_found")
[ -n "$r22" ] && hit "R22 spec §16 verification id with no owning test or gate" "$r22"

# Seeded self-test (R21 shape): a scratch tree proves each ownership class
# accepts what it must and, above all, that the gate FAILS when the owning test
# disappears and only its tag is left behind.
_r22_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r22_dir" ] || [ ! -d "$_r22_dir" ]; then
    hit "R22-selftest" "R22 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r22_dir"' EXIT
    printf '/* USA-77 partial: owned. */\nvoid test_owned(void) {\n}\nRUN_TEST(test_owned);\n' \
        >"$_r22_dir/test_owned.c"
    printf '/* USA-77 owned via wrapped registration. */\nvoid test_wrapped(void) {\n}\n/* USA-77 owned via bare call. */\nvoid test_bare(void) {\n}\nvoid run_all(void) {\n    RUN_TEST(\n        test_wrapped);\n    test_bare();\n}\n' \
        >"$_r22_dir/test_wrapped.c"
    printf '/* USA-77 tagged, defined, but its RUN_TEST line was deleted. */\nvoid test_unregistered(void) {\n}\n' \
        >"$_r22_dir/test_unregistered.c"
    printf '/* USA-77 tagged, but its registration is compiled out. */\nvoid test_if0(void) {\n}\nvoid run_all(void) {\n#if 0\n    RUN_TEST(test_if0);\n#endif\n}\n' \
        >"$_r22_dir/test_if0.c"
    printf '/* USA-77 tagged, but its registration is inside a nested dead region. */\nvoid test_if0_nested(void) {\n}\nvoid run_all(void) {\n#if 0\n#ifdef _WIN32\n    RUN_TEST(test_if0_nested);\n#endif\n#endif\n}\n' \
        >"$_r22_dir/test_if0_nested.c"
    printf '/* USA-77 tagged, but its registration is commented out. */\nvoid test_blockcomment(void) {\n}\nvoid run_all(void) {\n    /* RUN_TEST(test_blockcomment); */\n    /*\n    test_blockcomment();\n    */\n    // RUN_TEST(test_blockcomment);\n}\n' \
        >"$_r22_dir/test_blockcomment.c"
    printf '/* USA-77 owned via the LIVE else branch of an #if 0. */\nvoid test_if0_else(void) {\n}\nvoid run_all(void) {\n#if 0\n    test_never();\n#else\n    RUN_TEST(test_if0_else);\n#endif\n}\n' \
        >"$_r22_dir/test_if0_else.c"
    printf '/* USA-77 tagged, but its registration is in an #elif 0 branch. */\nvoid test_elif0(void) {\n}\nvoid run_all(void) {\n#if 0\n    test_never();\n#elif 0\n    RUN_TEST(test_elif0);\n#endif\n}\n' \
        >"$_r22_dir/test_elif0.c"
    printf '/* USA-77 owned; a string literal above it merely CONTAINS a comment opener. */\nvoid test_string_open(void) {\n}\nvoid run_all(void) {\n    const char *glob = "dir/*";\n    RUN_TEST(test_string_open);\n}\n' \
        >"$_r22_dir/test_string_open.c"
    printf '/* USA-77 owned; the registration shares its line with a UNC string literal. */\nvoid test_string_slashes(void) {\n}\nvoid run_all(void) {\n    note("//server/share"); RUN_TEST(test_string_slashes);\n}\n' \
        >"$_r22_dir/test_string_slashes.c"
    printf '/* USA-77: the test that owned this was deleted. */\n' \
        >"$_r22_dir/test_orphan.c"
    printf 'void helper(void) { /* USA-77 in production prose */ }\n' \
        >"$_r22_dir/prod.c"
    printf '# USA-77 named in a build file with no owner marker\n' \
        >"$_r22_dir/note.cmake"
    printf '# USA-78 owner: gate -- proven by a build check\n' \
        >"$_r22_dir/gate.cmake"

    [ "$(_usa_test_owned "$_r22_dir/test_owned.c")" = "USA-77" ] ||
        hit "R22-selftest" "R22 failed to credit a tag adjacent to its owning test"
    [ "$(_usa_test_owned "$_r22_dir/test_wrapped.c")" = "USA-77" ] ||
        hit "R22-selftest" "R22 failed to credit a wrapped RUN_TEST or bare-call registration"
    [ -n "$(_usa_test_owned "$_r22_dir/test_unregistered.c")" ] &&
        hit "R22-selftest" "R22 credited a defined-but-unregistered test (deleted RUN_TEST line)"
    [ -n "$(_usa_test_owned "$_r22_dir/test_if0.c")" ] &&
        hit "R22-selftest" "R22 credited a registration inside an #if 0 region"
    [ -n "$(_usa_test_owned "$_r22_dir/test_if0_nested.c")" ] &&
        hit "R22-selftest" "R22 credited a registration inside a nested #if 0 region"
    [ -n "$(_usa_test_owned "$_r22_dir/test_blockcomment.c")" ] &&
        hit "R22-selftest" "R22 credited a commented-out registration"
    [ "$(_usa_test_owned "$_r22_dir/test_if0_else.c")" = "USA-77" ] ||
        hit "R22-selftest" "R22 dropped a registration in the LIVE #else branch of an #if 0"
    [ -n "$(_usa_test_owned "$_r22_dir/test_elif0.c")" ] &&
        hit "R22-selftest" "R22 credited a registration inside an #elif 0 branch"
    [ "$(_usa_test_owned "$_r22_dir/test_string_open.c")" = "USA-77" ] ||
        hit "R22-selftest" "R22 read a comment opener inside a string literal as a real comment"
    [ "$(_usa_test_owned "$_r22_dir/test_string_slashes.c")" = "USA-77" ] ||
        hit "R22-selftest" "R22 read // inside a string literal as a line comment"
    [ -n "$(_usa_test_owned "$_r22_dir/test_orphan.c")" ] &&
        hit "R22-selftest" "R22 credited a tag whose owning test disappeared"
    [ -n "$(_usa_test_owned "$_r22_dir/prod.c")" ] &&
        hit "R22-selftest" "R22 credited a production comment as a test owner"
    [ -n "$(_usa_gate_owned "$_r22_dir/note.cmake")" ] &&
        hit "R22-selftest" "R22 credited a build-file note with no owner: gate marker"
    [ "$(_usa_gate_owned "$_r22_dir/gate.cmake")" = "USA-78" ] ||
        hit "R22-selftest" "R22 failed to credit an explicit gate owner"
    rm -rf "$_r22_dir"
    trap - EXIT
fi
if [ -n "$(_usa_missing "$(_usa_ids)")" ]; then
    hit "R22-selftest" "R22 detector false-positives on a fully covered id set"
fi

# 23. Spec §4.3 determinism: no locale-dependent byte classification or folding
#     in shipping code. The <ctype.h>/<wctype.h> case and character-class
#     functions consult the active LC_CTYPE, so the same path text, id text, or
#     token would land in different equivalence classes depending on the locale
#     the process happens to run under. packer/src/tp_ascii.h is the shared
#     replacement and needs no exception here -- it classifies by explicit ASCII
#     range and calls nothing. isdigit/isxdigit are deliberately NOT banned:
#     C17 §7.4.1.5 and §7.4.1.12 pin them to the decimal / hexadecimal digits in
#     every locale, so they carry no locale input. A legit exception carries a
#     "boundary-ok:" note on the same line.
_locale_ctype='(^|[^A-Za-z0-9_])(is|to)w?(alpha|alnum|upper|lower|space|punct|print|graph|cntrl|blank)[[:space:]]*\('
_locale_ctype_scan() {
    grep -nE "$_locale_ctype" "$@" 2>/dev/null | grep -v 'boundary-ok:'
}
r23=$(_locale_ctype_scan $(shipping_srcs))
[ -n "$r23" ] && hit "R23 locale-dependent ctype in shipping code" "$r23"

_r23_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r23_dir" ] || [ ! -d "$_r23_dir" ]; then
    hit "R23-selftest" "R23 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r23_dir"' EXIT
    printf '    if (isalpha((unsigned char)p[0]) && p[1] == 0x3A) {\n' \
        >"$_r23_dir/seeded_classify.c"
    printf '    return tolower(a) == tolower(b);\n' >"$_r23_dir/seeded_fold.c"
    printf '    return towupper(wc);\n' >"$_r23_dir/seeded_wide.c"
    printf '    return tp_ascii_tolower(a) == tp_ascii_tolower(b) &&\n           tp_ascii_is_alpha(a);\n' \
        >"$_r23_dir/shared_fold.c"
    printf '    if (!isdigit(byte) && !(byte >= 0x61 && byte <= 0x66)) {\n' \
        >"$_r23_dir/digit_grammar.c"
    [ -z "$(_locale_ctype_scan "$_r23_dir/seeded_classify.c")" ] &&
        hit "R23-selftest" "R23 failed to catch a seeded locale classification"
    [ -z "$(_locale_ctype_scan "$_r23_dir/seeded_fold.c")" ] &&
        hit "R23-selftest" "R23 failed to catch a seeded locale case fold"
    [ -z "$(_locale_ctype_scan "$_r23_dir/seeded_wide.c")" ] &&
        hit "R23-selftest" "R23 failed to catch a seeded wide-character locale fold"
    [ -n "$(_locale_ctype_scan "$_r23_dir/shared_fold.c")" ] &&
        hit "R23-selftest" "R23 false-positives on the shared ASCII fold"
    [ -n "$(_locale_ctype_scan "$_r23_dir/digit_grammar.c")" ] &&
        hit "R23-selftest" "R23 false-positives on the locale-independent digit predicates"
    rm -rf "$_r23_dir"
    trap - EXIT
fi

# 24. Test seams are not a shipping surface. `<owner>__test_<name>` marks a symbol
#     that exists only so a test can observe or break its owner. Compiled into the
#     shipped library it is an unaudited entry point any consumer can call, it
#     widens the internal contract, and it hides which functions are real API --
#     a fault-injection one also leaves the production path carrying a branch whose
#     only purpose is to fail. Every such symbol must therefore sit inside a live
#     `#ifdef TP_ENABLE_TEST_SEAMS` region, or in a translation unit that refuses to
#     compile without the define (the `#ifndef TP_ENABLE_TEST_SEAMS` + `#error` file
#     guard, e.g. packer/src/tp_test_seams.h). Consumers recompile the owning TU
#     with the define via tp_add_test's SEAMS keyword.
#     Complement to cmake A6, which pins two NAMED job-owner seams to their fences:
#     A6 proves those specific seams stay fenced, this proves no unfenced seam
#     appears anywhere in shipping code.
#     A compound guard (`#if defined(TP_ENABLE_TEST_SEAMS) || X`) is NOT a fence --
#     it can compile the seam into every build -- and neither is the `#else`/`#elif`
#     branch of one, matching A6's walker.
_seam_fence_awk='
FNR == 1 { in_block = 0; depth = 0; split("", seam); seam_only = 0; pending = 0 }
{
    line = $0
    out = ""
    i = 1
    n = length(line)
    while (i <= n) {
        c = substr(line, i, 2)
        if (in_block) {
            if (c == "*/") { in_block = 0; i += 2 } else { i++ }
            continue
        }
        if (c == "/*") { in_block = 1; i += 2; continue }
        if (c == "//") { break }
        out = out substr(line, i, 1)
        i++
    }
    line = out
    trimmed = line
    sub(/^[ \t]+/, "", trimmed)
    sub(/[ \t\r]+$/, "", trimmed)
    if (pending && trimmed != "") {
        if (trimmed ~ /^#[ \t]*error/) { seam_only = 1 }
        pending = 0
    }
    if (trimmed ~ /^#[ \t]*(if|ifdef|ifndef)([^A-Za-z0-9_]|$)/) {
        depth++
        seam[depth] = (trimmed ~ /^#[ \t]*ifdef[ \t]+TP_ENABLE_TEST_SEAMS$/ ||
                       trimmed ~ /^#[ \t]*if[ \t]+defined[ \t]*\([ \t]*TP_ENABLE_TEST_SEAMS[ \t]*\)$/) ? 1 : 0
        if (trimmed ~ /^#[ \t]*ifndef[ \t]+TP_ENABLE_TEST_SEAMS$/) { pending = 1 }
        next
    }
    if (trimmed ~ /^#[ \t]*endif/) { seam[depth] = 0; if (depth > 0) depth--; next }
    if (trimmed ~ /^#[ \t]*(else|elif)/) { seam[depth] = 0; next }
    if (seam_only) next
    for (d = 1; d <= depth; d++) { if (seam[d]) next }
    rest = line
    while (match(rest, /[A-Za-z_][A-Za-z0-9_]*__test_[A-Za-z0-9_]*/)) {
        printf "%s:%d:%s\n", FILENAME, FNR, substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
    }
}
'
# DEBT, not design. Each name below is a seam that predates this rule and whose
# owning component is not fenced yet. The list is exact -- full symbol names, no
# prefixes, no patterns -- so a NEW unfenced seam, including one in an
# already-listed component, is still a hit. The session, project, and
# build-worker families are already fenced; the
# owners still owed a fence are tp_diff/tp_history, tp_idset, tp_image, tp_journal,
# tp_model, tp_op, tp_recovery, tp_txn, tp_validate, the CLI pack-fault binary's
# tp_export_run arming call, and the GUI's gui_project__test_session.
# Shrink this list; never grow it.
_seam_fence_allowed='gui_project__test_session
tp_diff__test_alloc_count
tp_diff__test_reset_alloc_count
tp_diff__test_set_alloc_fail
tp_export_run__test_set_report_alloc_fail
tp_history__test_fail_next_reserve
tp_history__test_set_limits
tp_idset__test_force_bucket
tp_idset__test_probe_reset
tp_idset__test_probe_take
tp_image__test_decode_count
tp_image__test_reset_decode_count
tp_journal__test_fail_next_metadata_materialize
tp_journal__test_has_valid_record_after
tp_journal__test_recovery_copy_stats
tp_journal__test_recovery_ops_borrow_raw
tp_journal__test_set_file_limit
tp_journal__test_set_record_limit
tp_model__test_set_revision
tp_op__test_apply_count_publish
tp_op__test_apply_count_reset
tp_op__test_apply_count_take
tp_op__test_set_alloc_fail
tp_recovery__test_candidate_insert
tp_recovery__test_craft_metadata_journal
tp_recovery__test_fail_next_live_retire_cleanup
tp_recovery__test_fail_next_quarantine_unlink
tp_recovery__test_fail_next_resolve_verify
tp_recovery__test_hold_foreign_lock
tp_recovery__test_peek_candidate
tp_recovery__test_release_foreign_lock
tp_recovery__test_session_attach_at
tp_txn__test_complexity_reset
tp_txn__test_count_op_walk
tp_txn__test_encode_stats_reset
tp_txn__test_error_allocations
tp_txn__test_fail_next_request_encode
tp_txn__test_json_precheck
tp_txn__test_last_measure_allocations
tp_txn__test_op_walk_steps
tp_txn__test_request_encode_calls
tp_txn__test_set_add_error_fail
tp_txn__test_set_result_echo_fail
tp_validate__test_fail_sprite_index
tp_validate__test_set_alloc_fail
tp_validate__test_work_get
tp_validate__test_work_reset'
# The allowance list reaches awk through the ENVIRONMENT, not `-v`: a `-v`
# assignment is a single lexical string, and awks other than gawk reject an
# embedded newline in one ("newline in string"), which kills the program before
# it reads a line -- a scan that cannot run must not look like a clean scan.
# stderr is deliberately NOT discarded here for the same reason: an awk that
# refuses the program has to be visible.
_seam_fence_scan() {
    awk "$_seam_fence_awk" "$@" |
        SEAM_FENCE_ALLOWED="$_seam_fence_allowed" awk -F: '
            BEGIN {
                n = split(ENVIRON["SEAM_FENCE_ALLOWED"], a, "\n")
                for (i = 1; i <= n; i++) ok[a[i]] = 1
            }
            !($3 in ok)'
}
r24=$(_seam_fence_scan $(shipping_srcs))
[ -n "$r24" ] && hit "R24 test seam outside a TP_ENABLE_TEST_SEAMS fence" "$r24"

_r24_dir=$(mktemp -d 2>/dev/null)
if [ -z "$_r24_dir" ] || [ ! -d "$_r24_dir" ]; then
    hit "R24-selftest" "R24 self-test could not create a scratch dir (mktemp failed)"
else
    trap 'rm -rf "$_r24_dir"' EXIT
    printf 'void tp_thing__test_fail_next(void);\n' >"$_r24_dir/seeded_plain.h"
    printf '#ifdef TP_ENABLE_TEST_SEAMS\nvoid tp_thing__test_fail_next(void);\n#endif\n' \
        >"$_r24_dir/fenced.h"
    printf '#ifdef TP_ENABLE_TEST_SEAMS\n#ifdef _WIN32\nvoid tp_thing__test_fail_next(void);\n#endif\n#endif\n' \
        >"$_r24_dir/fenced_nested.h"
    printf '#if defined(TP_ENABLE_TEST_SEAMS) || defined(TP_OTHER)\nvoid tp_thing__test_fail_next(void);\n#endif\n' \
        >"$_r24_dir/compound.h"
    printf '#ifdef TP_ENABLE_TEST_SEAMS\nvoid a(void);\n#else\nvoid tp_thing__test_fail_next(void);\n#endif\n' \
        >"$_r24_dir/else_branch.h"
    printf '#ifndef TP_ENABLE_TEST_SEAMS\n#error "seam-only header"\n#endif\nvoid tp_thing__test_fail_next(void);\n' \
        >"$_r24_dir/seam_only.h"
    printf '/* tp_thing__test_fail_next is named here in prose only. */\nvoid tp_thing_real(void);\n' \
        >"$_r24_dir/comment.h"
    printf 'void tp_txn__test_complexity_reset(void);\n' >"$_r24_dir/allowed.h"
    [ -z "$(_seam_fence_scan "$_r24_dir/seeded_plain.h")" ] &&
        hit "R24-selftest" "R24 failed to catch an unfenced seam declaration"
    [ -n "$(_seam_fence_scan "$_r24_dir/fenced.h")" ] &&
        hit "R24-selftest" "R24 false-positives on a properly fenced seam"
    [ -n "$(_seam_fence_scan "$_r24_dir/fenced_nested.h")" ] &&
        hit "R24-selftest" "R24 false-positives on a seam nested inside its fence"
    [ -z "$(_seam_fence_scan "$_r24_dir/compound.h")" ] &&
        hit "R24-selftest" "R24 accepted a compound #if condition as a seam fence"
    [ -z "$(_seam_fence_scan "$_r24_dir/else_branch.h")" ] &&
        hit "R24-selftest" "R24 accepted the #else branch of a seam fence as fenced"
    [ -n "$(_seam_fence_scan "$_r24_dir/seam_only.h")" ] &&
        hit "R24-selftest" "R24 false-positives on a header that #errors without the define"
    [ -n "$(_seam_fence_scan "$_r24_dir/comment.h")" ] &&
        hit "R24-selftest" "R24 read a seam name in a comment as a declaration"
    [ -n "$(_seam_fence_scan "$_r24_dir/allowed.h")" ] &&
        hit "R24-selftest" "R24 ignored its exact debt allowance"
    rm -rf "$_r24_dir"
    trap - EXIT
fi

# 25. The GUI has one explicit host driver. Only gui_actions.c advances the
#     private project FSM; main/dev adapters advance gui_actions_step, and views
#     can only include the typed ingress/passive-state header.
_gui_project_step_call='(^|[^A-Za-z0-9_])gui_project_step[[:space:]]*\('
r25_project=$(find apps/gui -maxdepth 1 -type f \
    \( -name 'gui*.c' -o -name 'main.c' \) \
    ! -name 'gui_actions.c' ! -name 'gui_project.c' \
    ! -name 'gui_selftest.c' |
    xargs grep -nE "$_gui_project_step_call" 2>/dev/null)
[ -n "$r25_project" ] &&
    hit "R25 multiple GUI project FSM drivers" "$r25_project"

_gui_project_test_driver_include='#include[[:space:]]+"gui_project_test_driver[.]h"'
_gui_test_fixture_include='#include[[:space:]]+"test_gui_[A-Za-z0-9_]*[.]h"'
r25_shipping_test_driver=$(find apps/gui -maxdepth 1 -type f \
    \( -name 'gui*.c' -o -name 'main.c' \) \
    ! -name 'gui_selftest.c' |
    xargs grep -nE "${_gui_project_test_driver_include}|${_gui_test_fixture_include}" 2>/dev/null)
[ -n "$r25_shipping_test_driver" ] &&
    hit "R25 shipping GUI source imports a test driver or fixture" \
        "$r25_shipping_test_driver"

_gui_view_driver_include='#include[[:space:]]+"gui_actions_(driver|dev)[.]h"'
r25_view=$(grep -nE "$_gui_view_driver_include" \
    apps/gui/gui_view_*.c apps/gui/gui_view_*.h 2>/dev/null)
[ -n "$r25_view" ] &&
    hit "R25 view includes a host-driving actions contract" "$r25_view"

# A production helper header can otherwise smuggle the project driver into a
# shipping TU without putting the forbidden call in that TU. Only the concrete
# driver declaration, the two owning internal surfaces, and the test-only
# inline driver may name/import it.
_gui_project_driver_header_include='#include[[:space:]]+"gui_project_(driver|test_driver)[.]h"'
r25_header=$(find apps/gui -maxdepth 1 -type f -name '*.h' \
    ! -name 'test_*.h' \
    ! -name 'gui_actions_internal.h' \
    ! -name 'gui_project_driver.h' \
    ! -name 'gui_project_internal.h' \
    ! -name 'gui_project_test_driver.h' |
    xargs grep -nE "${_gui_project_step_call}|${_gui_project_driver_header_include}|${_gui_test_fixture_include}" 2>/dev/null)
[ -n "$r25_header" ] &&
    hit "R25 production header introduces a project FSM driver" "$r25_header"

if ! printf '    gui_project_step(&result, &error);\n' |
    grep -qE "$_gui_project_step_call"; then
    hit "R25-selftest" "R25 failed to catch a seeded second project driver"
fi
if ! printf '#include "gui_actions_driver.h"\n' |
    grep -qE "$_gui_view_driver_include"; then
    hit "R25-selftest" "R25 failed to catch a seeded view-header driver include"
fi
if ! printf '#include "gui_project_test_driver.h"\nstatic bool second_driver(void) { return gui_project_test_new(); }\n' |
    grep -qE "$_gui_project_test_driver_include"; then
    hit "R25-selftest" "R25 failed to catch a seeded wrapper-based project driver"
fi
if ! printf '#include "test_gui_action_trace_fixture.h"\nstatic bool second_driver(void) { return gui_project_test_new(); }\n' |
    grep -qE "$_gui_test_fixture_include"; then
    hit "R25-selftest" "R25 failed to catch a seeded transitive GUI test fixture"
fi

if [ "$fail" -eq 0 ]; then
    say "boundaries OK"
fi
exit "$fail"
