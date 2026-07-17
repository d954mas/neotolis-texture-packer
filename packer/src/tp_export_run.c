#include "tp_core/tp_export_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_scan.h"
#include "tp_project_mutation_internal.h"
#include "tp_session_internal.h"

#define TP_RUN_PATH_MAX TP_IDENTITY_PATH_MAX

struct tp_export_snapshot_job {
    tp_project *project;
    char *work_dir;
    bool dry_run;
};

static bool run_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':';
}

/* One shared pack run: an effective-settings signature + its packed result and
 * the normalized view every target in the group exports from. */
typedef struct {
    tp_pack_settings eff;
    tp_result *result;
    tp_export_prepared prep;
} run_group;

/* Builds the normalize options for an atlas (target-independent). Explicit
 * animations and rename overrides are borrowed from the project; tp_normalize
 * copies what it keeps. `anims`/`ovs` are caller-provided arena buffers sized to
 * a->animation_count / a->sprite_count (either may be NULL when its count is 0).
 *
 * Renames: a project sprite's `name` is its export KEY (ext-stripped, folder-
 * kept), but tp_normalize keys overrides on the RAW packer name -- so map
 * key -> raw via the shared export-key policy (tp_sprite_export_key) over the
 * packed descs. A stale rename whose key matches no packed sprite is simply
 * skipped (nothing to rename; not an error -- L-4: renames do not dangle). The
 * emitted entries borrow: raw_name points into `sprites` (the caller's desc
 * array) and final_name into ps->rename; both outlive every tp_normalize call in
 * this run, and final_name is duped by tp_normalize. */
static tp_status build_norm_opts(const tp_project_atlas *a, const tp_pack_sprite_desc *sprites, int sprite_count,
                                 tp_export_anim_in *anims, tp_export_name_override *ovs,
                                 tp_export_sprite_ref_in *refs, tp_arena *arena,
                                 tp_normalize_opts *out, tp_error *err) {
    tp_normalize_opts_defaults(out);
    for (int i = 0; i < a->animation_count; i++) {
        const tp_project_anim *pa = &a->animations[i];
        anims[i].id = pa->name; /* export "id" is the animation's logical name (id/name split) */
        /* Frames remain canonical {source, key} records through normalization. The
         * raw-packed-name -> canonical-ref table below resolves them only after pack,
         * avoiding the legacy display-name bridge and cross-source collisions. */
        tp_export_frame_ref *fnames = NULL;
        if (pa->frame_count > 0) {
            fnames = (tp_export_frame_ref *)tp_arena_alloc(
                arena, (size_t)pa->frame_count * sizeof *fnames);
            if (!fnames) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (frame names)");
            }
            for (int f = 0; f < pa->frame_count; f++) {
                fnames[f].source_id = pa->frames[f].source_ref;
                fnames[f].source_key = pa->frames[f].src_key;
            }
        }
        anims[i].frames = fnames;
        anims[i].frame_count = pa->frame_count;
        anims[i].fps = pa->fps;
        anims[i].playback = pa->playback;
        anims[i].flip_h = pa->flip_h;
        anims[i].flip_v = pa->flip_v;
    }
    out->animations = anims;
    out->animation_count = a->animation_count;

    int oc = 0;
    for (int d = 0; d < sprite_count; d++) {
        refs[d].raw_name = sprites[d].name;
        refs[d].source_id = sprites[d].source_id;
        refs[d].source_key = sprites[d].source_key;
        const tp_project_sprite *ps = tp_project_atlas_find_sprite_by_source_key(
            (tp_project_atlas *)a, sprites[d].source_id, sprites[d].source_key);
        ovs[oc].raw_name = sprites[d].name;
        ovs[oc].final_name = ps && ps->rename && ps->rename[0] != '\0'
                                 ? ps->rename
                                 : (sprites[d].logical_name
                                        ? sprites[d].logical_name
                                        : sprites[d].name);
        oc++;
    }
    out->overrides = ovs;
    out->override_count = oc;
    out->sprite_refs = refs;
    out->sprite_ref_count = sprite_count;
    return TP_STATUS_OK;
}

static tp_status unknown_exporter(const char *id, tp_error *err) {
    char known[256];
    size_t used = 0;
    known[0] = '\0';
    for (int i = 0; i < tp_exporter_count(); i++) {
        const tp_exporter *e = tp_exporter_at(i);
        int n = snprintf(known + used, sizeof known - used, "%s%s", (i == 0) ? "" : ", ", e->id);
        if (n < 0 || (size_t)n >= sizeof known - used) {
            break;
        }
        used += (size_t)n;
    }
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_run: unknown exporter '%s' (known: %s)", id, known);
}

/* ------------------------------------------------------------------ */
/* report assembly (only when the caller wants a report)              */
/* ------------------------------------------------------------------ */

/* Collects arena-duped output paths into a pre-sized array. `files == NULL` is a
 * counting pass (only `count` moves); the fill pass sets `files`/`cap`. */
typedef struct {
    tp_arena *arena;
    const char **files;
    int count;
    int cap;
    bool oom;
} file_collect;

static void file_count_sink(void *ud, const char *path) {
    (void)path;
    ((file_collect *)ud)->count++;
}

static void file_fill_sink(void *ud, const char *path) {
    file_collect *fc = (file_collect *)ud;
    if (fc->count >= fc->cap) {
        fc->count++; /* keep counting so a mismatch is visible; never write past end */
        return;
    }
    char *dup = tp_arena_strdup(fc->arena, path);
    if (!dup) {
        fc->oom = true;
        return;
    }
    fc->files[fc->count++] = dup;
}

/* Enumerates a target's produced files (exporter list_outputs, or the common
 * "<base>.<ext>" + page PNGs default) into `sink`. */
static void list_target_outputs(const tp_exporter *exp, const tp_export_prepared *prep, const char *out_base,
                                tp_export_path_sink sink, void *ud) {
    if (exp->list_outputs) {
        exp->list_outputs(prep, out_base, sink, ud);
        return;
    }
    char path[TP_RUN_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s.%s", out_base, exp->extension ? exp->extension : "");
    if (n > 0 && (size_t)n < sizeof path) {
        sink(ud, path);
    }
    tp_export_list_page_files(prep->result, out_base, sink, ud);
}

/* Enumerates the target's output paths (two passes: count then fill) into an
 * arena-owned array, writing out_files + out_count. Used for both the wet-path
 * written_files and the dry-path would_write (identical list; dry just never
 * writes). Returns TP_STATUS_OOM on allocation failure. */
static tp_status collect_output_files(const tp_exporter *exp, const tp_export_prepared *prep, const char *out_base,
                                      tp_arena *arena, const char *const **out_files, int *out_count) {
    file_collect c = {.arena = arena};
    list_target_outputs(exp, prep, out_base, file_count_sink, &c);
    int n = c.count;
    if (n <= 0) {
        *out_files = NULL;
        *out_count = 0;
        return TP_STATUS_OK;
    }
    const char **arr = (const char **)tp_arena_alloc(arena, (size_t)n * sizeof(char *));
    if (!arr) {
        return TP_STATUS_OOM;
    }
    file_collect f = {.arena = arena, .files = arr, .cap = n};
    list_target_outputs(exp, prep, out_base, file_fill_sink, &f);
    if (f.oom) {
        return TP_STATUS_OOM;
    }
    *out_files = arr;
    *out_count = (f.count < n) ? f.count : n;
    return TP_STATUS_OK;
}

/* Builds one pack-run report entry (pages + occupancy) from a packed result. */
static tp_status fill_run_report(tp_export_report_run *run, const tp_result *r, tp_arena *arena) {
    run->sprite_count = r->sprite_count;
    run->page_count = r->page_count;
    run->pages = NULL;
    if (r->page_count > 0) {
        run->pages = (tp_export_report_page *)tp_arena_alloc(arena, (size_t)r->page_count * sizeof(*run->pages));
        if (!run->pages) {
            return TP_STATUS_OOM;
        }
    }
    /* Accumulate covered area per page from ORIGINAL placements only (an alias
     * shares its original's pixels; counting both would double-count). */
    for (int p = 0; p < r->page_count; p++) {
        run->pages[p].index = p;
        run->pages[p].w = r->pages[p].w;
        run->pages[p].h = r->pages[p].h;
        run->pages[p].occupancy_pct = 0.0; /* used as a covered-area accumulator first */
    }
    for (int i = 0; i < r->sprite_count; i++) {
        const tp_sprite *s = &r->sprites[i];
        if (s->alias_of != -1) {
            continue;
        }
        if (s->page < 0 || s->page >= r->page_count) {
            continue;
        }
        run->pages[s->page].occupancy_pct += (double)s->frame.w * (double)s->frame.h;
    }
    for (int p = 0; p < r->page_count; p++) {
        double page_area = (double)r->pages[p].w * (double)r->pages[p].h;
        run->pages[p].occupancy_pct = (page_area > 0.0) ? (run->pages[p].occupancy_pct / page_area * 100.0) : 0.0;
    }
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* run                                                                */
/* ------------------------------------------------------------------ */

tp_status tp_export_run_ex(const tp_project *project, int atlas_index, const tp_pack_sprite_desc *sprites,
                           int sprite_count, const char *work_dir, tp_arena *arena, tp_export_notices *notices,
                           int *out_pack_runs, const tp_export_run_opts *opts, tp_error *err) {
    tp_export_report *report = opts ? opts->report : NULL;
    const bool dry_run = opts && opts->dry_run;
    if (report) {
        memset(report, 0, sizeof *report);
        report->dry_run = dry_run;
    }
    if (out_pack_runs) {
        *out_pack_runs = 0;
    }
    if (!project || !sprites || sprite_count <= 0 || !work_dir || !arena) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_run: NULL/empty required argument");
    }
    if (atlas_index < 0 || atlas_index >= project->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_export_run: atlas index %d out of range", atlas_index);
    }
    const tp_project_atlas *a = &project->atlases[atlas_index];

    tp_pack_settings base;
    tp_status st = tp_project_atlas_to_settings(project, atlas_index, &base, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    base.work_dir = work_dir;
    base.sprites = sprites;
    base.sprite_count = sprite_count;

    /* normalize options (shared across all targets of this atlas). */
    tp_export_anim_in *anims = NULL;
    if (a->animation_count > 0) {
        anims = (tp_export_anim_in *)tp_arena_alloc(arena, (size_t)a->animation_count * sizeof(tp_export_anim_in));
        if (!anims) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (anim opts)");
        }
    }
    tp_export_name_override *ovs = NULL;
    if (sprite_count > 0) {
        ovs = (tp_export_name_override *)tp_arena_alloc(arena, (size_t)sprite_count * sizeof(tp_export_name_override));
        if (!ovs) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (rename opts)");
        }
    }
    tp_export_sprite_ref_in *refs = (tp_export_sprite_ref_in *)tp_arena_alloc(
        arena, (size_t)sprite_count * sizeof *refs);
    if (!refs) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (sprite refs)");
    }
    tp_normalize_opts nopts;
    st = build_norm_opts(a, sprites, sprite_count, anims, ovs, refs, arena,
                         &nopts, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    if (a->target_count == 0) {
        return TP_STATUS_OK; /* nothing enabled to export */
    }
    run_group *groups = (run_group *)tp_arena_alloc(arena, (size_t)a->target_count * sizeof(run_group));
    int *target_group = (int *)tp_arena_alloc(arena, (size_t)a->target_count * sizeof(int));
    const char **out_bases = (const char **)tp_arena_alloc(arena, (size_t)a->target_count * sizeof(char *));
    int *rtidx = (int *)tp_arena_alloc(arena, (size_t)a->target_count * sizeof(int));
    if (!groups || !target_group || !out_bases || !rtidx) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (groups)");
    }
    int group_count = 0;

    /* Map enabled targets to report-target slots (in target order). */
    int enabled_count = 0;
    for (int t = 0; t < a->target_count; t++) {
        rtidx[t] = a->targets[t].enabled ? enabled_count++ : -1;
    }
    if (report && enabled_count > 0) {
        report->targets =
            (tp_export_report_target *)tp_arena_alloc(arena, (size_t)enabled_count * sizeof(*report->targets));
        if (!report->targets) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (report targets)");
        }
        memset(report->targets, 0, (size_t)enabled_count * sizeof(*report->targets));
        report->target_count = enabled_count;
    }

    /* first_fail tracks the first per-target failure on the report path (the return
     * value); on the report==NULL path a failure returns immediately below. */
    tp_status first_fail = TP_STATUS_OK;

    /* Phase 1: resolve exporters + out paths, compute effective settings, pack once
     * per distinct effective-settings signature (SUMMARY.md §5h shared run). */
    for (int t = 0; t < a->target_count; t++) {
        const tp_project_target *tg = &a->targets[t];
        target_group[t] = -1;
        out_bases[t] = NULL;
        if (!tg->enabled) {
            continue;
        }
        tp_export_report_target *rt = (report && rtidx[t] >= 0) ? &report->targets[rtidx[t]] : NULL;
        if (rt) {
            rt->exporter_id = tp_arena_strdup(arena, tg->exporter_id ? tg->exporter_id : "");
            rt->pack_run = -1;
        }

        const tp_exporter *exp = tp_exporter_find(tg->exporter_id);
        if (!exp) {
            tp_status ust = unknown_exporter(tg->exporter_id, err);
            if (!report) {
                return ust;
            }
            if (rt) {
                rt->error = tp_arena_strdup(arena, err && err->msg[0] ? err->msg : "unknown exporter");
            }
            if (first_fail == TP_STATUS_OK) {
                first_fail = ust;
            }
            target_group[t] = -2; /* failed: skip in phase 2 */
            continue;
        }

        char out_base[TP_RUN_PATH_MAX];
        st = tp_project_resolve_path(project, tg->out_path, out_base, sizeof out_base);
        if (st != TP_STATUS_OK) {
            tp_error_set(err, st, "tp_export_run: cannot resolve out_path '%s'", tg->out_path);
            if (!report) {
                return st;
            }
            if (rt) {
                rt->error = tp_arena_strdup(arena, err && err->msg[0] ? err->msg : "cannot resolve out_path");
            }
            if (first_fail == TP_STATUS_OK) {
                first_fail = st;
            }
            target_group[t] = -2;
            continue;
        }
        out_bases[t] = tp_arena_strdup(arena, out_base);
        if (!out_bases[t]) {
            if (report) {
                report->pack_failed = true;
            }
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (out path)");
        }
        if (rt) {
            rt->out_path = out_bases[t];
        }

        tp_pack_settings eff;
        st = tp_export_effective_settings(&base, &exp->caps, &eff);
        if (st != TP_STATUS_OK) {
            if (report) {
                report->pack_failed = true;
            }
            return tp_error_set(err, st, "tp_export_run: effective settings failed for '%s'", tg->exporter_id);
        }
        int g = -1;
        for (int k = 0; k < group_count; k++) {
            if (tp_export_settings_equal(&groups[k].eff, &eff)) {
                g = k;
                break;
            }
        }
        if (g < 0) {
            g = group_count++;
            groups[g].eff = eff;
            st = tp_pack(&eff, arena, &groups[g].result, err);
            if (st != TP_STATUS_OK) {
                if (report) {
                    report->pack_failed = true;
                }
                return st;
            }
            st = tp_normalize(groups[g].result, &nopts, arena, &groups[g].prep, err);
            if (st != TP_STATUS_OK) {
                if (report) {
                    report->pack_failed = true;
                }
                return st;
            }
        }
        target_group[t] = g;
        if (rt) {
            rt->pack_run = g;
        }
    }

    /* Phase 2: each ready target writes its files from its group's prepared. */
    for (int t = 0; t < a->target_count; t++) {
        if (target_group[t] < 0) {
            continue; /* disabled (-1) or failed in phase 1 (-2) */
        }
        const tp_project_target *tg = &a->targets[t];
        const tp_exporter *exp = tp_exporter_find(tg->exporter_id);
        tp_export_report_target *rt = (report && rtidx[t] >= 0) ? &report->targets[rtidx[t]] : NULL;

        const tp_export_prepared *prep = &groups[target_group[t]].prep;
        int nbefore = notices ? notices->count : 0;
        if (rt) {
            rt->notice_begin = nbefore;
        }

        if (dry_run) {
            /* No writes. The wet path's notices come from the writers, which do not
             * run here -- so predict every degradation instead (full axes: the packed
             * prep supplies the alias/multipage axes on top of the project-knowable
             * ones). ai-first.md item 6: dry-run reports the predicted losses. */
            st = tp_export_predict_loss(project, atlas_index, &exp->caps, exp->id, prep, notices, err);
            if (rt) {
                rt->notice_end = notices ? notices->count : nbefore;
            }
            if (st != TP_STATUS_OK) {
                if (!report) {
                    return st;
                }
                if (rt) {
                    rt->ok = false;
                    rt->error = tp_arena_strdup(arena, err && err->msg[0] ? err->msg : "predict-loss failed");
                }
                if (first_fail == TP_STATUS_OK) {
                    first_fail = st;
                }
                continue;
            }
            if (rt) {
                rt->ok = true;
                tp_status cst = collect_output_files(exp, prep, out_bases[t], arena, &rt->would_write,
                                                     &rt->would_write_count);
                if (cst != TP_STATUS_OK) {
                    return tp_error_set(err, cst, "tp_export_run: OOM (would-write report)");
                }
            }
            continue;
        }

        st = exp->write(prep, &exp->caps, out_bases[t], notices, err);
        if (rt) {
            rt->notice_end = notices ? notices->count : nbefore;
        }
        if (st != TP_STATUS_OK) {
            if (!report) {
                return st;
            }
            if (rt) {
                rt->ok = false;
                rt->error = tp_arena_strdup(arena, err && err->msg[0] ? err->msg : "export write failed");
            }
            if (first_fail == TP_STATUS_OK) {
                first_fail = st;
            }
            continue;
        }
        if (rt) {
            rt->ok = true;
            tp_status cst =
                collect_output_files(exp, prep, out_bases[t], arena, &rt->written_files, &rt->written_file_count);
            if (cst != TP_STATUS_OK) {
                return tp_error_set(err, cst, "tp_export_run: OOM (written-files report)");
            }
        }
    }

    if (report) {
        report->run_count = group_count;
        if (group_count > 0) {
            report->runs = (tp_export_report_run *)tp_arena_alloc(arena, (size_t)group_count * sizeof(*report->runs));
            if (!report->runs) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (report runs)");
            }
            for (int g = 0; g < group_count; g++) {
                tp_status fst = fill_run_report(&report->runs[g], groups[g].result, arena);
                if (fst != TP_STATUS_OK) {
                    return tp_error_set(err, fst, "tp_export_run: OOM (report pages)");
                }
            }
        }
    }

    if (out_pack_runs) {
        *out_pack_runs = group_count;
    }
    return report ? first_fail : TP_STATUS_OK;
}

tp_status tp_export_run(const tp_project *project, int atlas_index, const tp_pack_sprite_desc *sprites,
                        int sprite_count, const char *work_dir, tp_arena *arena, tp_export_notices *notices,
                        int *out_pack_runs, tp_error *err) {
    return tp_export_run_ex(project, atlas_index, sprites, sprite_count, work_dir, arena, notices, out_pack_runs, NULL,
                            err);
}

tp_status tp_export_snapshot_job_create(const tp_session_snapshot *snapshot,
                                        const char *work_dir,
                                        tp_export_snapshot_job **out,
                                        tp_error *err) {
    return tp_export_snapshot_job_create_ex(snapshot, work_dir, NULL, out, err);
}

tp_status tp_export_snapshot_job_create_ex(const tp_session_snapshot *snapshot,
                                           const char *work_dir,
                                           const tp_export_snapshot_job_opts *opts,
                                           tp_export_snapshot_job **out,
                                           tp_error *err) {
    if (!snapshot || !work_dir || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export snapshot job requires snapshot, work dir, and output");
    }
    *out = NULL;
    tp_export_snapshot_job *job = calloc(1U, sizeof *job);
    if (!job) {
        return tp_error_set(err, TP_STATUS_OOM, "export snapshot job allocation failed");
    }
    job->project = tp_project_clone(tp_session_snapshot_project_internal(snapshot));
    const size_t work_dir_len = strlen(work_dir) + 1U;
    job->work_dir = malloc(work_dir_len);
    if (job->work_dir) {
        memcpy(job->work_dir, work_dir, work_dir_len);
    }
    if (!job->project || !job->work_dir) {
        tp_export_snapshot_job_destroy(job);
        return tp_error_set(err, TP_STATUS_OOM, "export snapshot job clone failed");
    }
    job->dry_run = opts && opts->dry_run;
    for (int ai = 0; opts && (opts->target_exporter_id || opts->out_dir) &&
                     ai < job->project->atlas_count; ++ai) {
        tp_project_atlas *atlas = &job->project->atlases[ai];
        for (int ti = 0; ti < atlas->target_count; ++ti) {
            tp_project_target *target = &atlas->targets[ti];
            const bool enabled = target->enabled &&
                (!opts || !opts->target_exporter_id ||
                 strcmp(target->exporter_id, opts->target_exporter_id) == 0);
            const char *out_path = target->out_path;
            char rerooted[TP_IDENTITY_PATH_MAX];
            if (opts && opts->out_dir && !run_path_is_absolute(out_path)) {
                const int n = snprintf(rerooted, sizeof rerooted, "%s/%s",
                                       opts->out_dir, out_path);
                if (n < 0 || (size_t)n >= sizeof rerooted) {
                    tp_export_snapshot_job_destroy(job);
                    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                        "export output path exceeds the supported limit");
                }
                out_path = rerooted;
            }
            tp_status status = tp_project_atlas_set_target(
                atlas, ti, target->exporter_id, out_path, enabled);
            if (status != TP_STATUS_OK) {
                tp_export_snapshot_job_destroy(job);
                return tp_error_set(err, status,
                                    "export snapshot target filter failed");
            }
        }
    }
    *out = job;
    return TP_STATUS_OK;
}

void tp_export_snapshot_job_destroy(tp_export_snapshot_job *job) {
    if (!job) {
        return;
    }
    tp_project_destroy(job->project);
    free(job->work_dir);
    free(job);
}

int tp_export_snapshot_job_atlas_count(const tp_export_snapshot_job *job) {
    return job && job->project ? job->project->atlas_count : 0;
}

tp_status tp_export_snapshot_job_atlas_info(const tp_export_snapshot_job *job,
                                            int atlas_index,
                                            tp_export_snapshot_atlas_info *out,
                                            tp_error *err) {
    if (!job || !job->project || !out || atlas_index < 0 ||
        atlas_index >= job->project->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export snapshot atlas index is out of range");
    }
    const tp_project_atlas *atlas = &job->project->atlases[atlas_index];
    memset(out, 0, sizeof *out);
    out->atlas_id = atlas->id;
    out->name = atlas->name;
    out->source_count = atlas->source_count;
    for (int ti = 0; ti < atlas->target_count; ++ti) {
        out->enabled_target_count += atlas->targets[ti].enabled ? 1 : 0;
    }
    return TP_STATUS_OK;
}

tp_status tp_export_snapshot_job_run_atlas(tp_export_snapshot_job *job,
                                           int atlas_index,
                                           tp_arena *arena,
                                           tp_export_notices *notices,
                                           int *out_pack_runs,
                                           int *out_missing_sources,
                                           tp_error *err) {
    return tp_export_snapshot_job_run_atlas_ex(
        job, atlas_index, arena, notices, NULL, out_pack_runs, NULL,
        out_missing_sources, err);
}

tp_status tp_export_snapshot_job_run_atlas_ex(tp_export_snapshot_job *job,
                                              int atlas_index,
                                              tp_arena *arena,
                                              tp_export_notices *notices,
                                              tp_export_report *report,
                                              int *out_pack_runs,
                                              int *out_sprite_count,
                                              int *out_missing_sources,
                                              tp_error *err) {
    if (!job || !job->project || atlas_index < 0 ||
        atlas_index >= job->project->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export snapshot atlas index is out of range");
    }
    const tp_project_atlas *atlas = &job->project->atlases[atlas_index];
    tp_pack_input input;
    tp_status status = tp_pack_input_build(job->project, atlas_index, &input, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (out_missing_sources) {
        *out_missing_sources = input.missing_sources;
    }
    if (out_sprite_count) {
        *out_sprite_count = input.count;
    }
    if (input.count == 0) {
        tp_pack_input_free(&input);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "atlas has no usable images");
    }
    for (int ti = 0; !job->dry_run && ti < atlas->target_count; ++ti) {
        if (!atlas->targets[ti].enabled) {
            continue;
        }
        char output_path[TP_RUN_PATH_MAX];
        status = tp_project_resolve_path(job->project,
                                         atlas->targets[ti].out_path,
                                         output_path,
                                         sizeof output_path);
        if (status != TP_STATUS_OK) {
            tp_pack_input_free(&input);
            return tp_error_set(err, status,
                                "save the project first (relative output paths need a project dir)");
        }
        tp_mkdirs_parent(output_path);
    }
    tp_export_run_opts run_opts = {.report = report, .dry_run = job->dry_run};
    status = tp_export_run_ex(job->project, atlas_index, input.descs, input.count,
                              job->work_dir, arena, notices, out_pack_runs,
                              report || job->dry_run ? &run_opts : NULL, err);
    tp_pack_input_free(&input);
    return status;
}
