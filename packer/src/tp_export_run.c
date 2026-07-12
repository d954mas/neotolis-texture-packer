#include "tp_core/tp_export_run.h"

#include <stdio.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"

#define TP_RUN_PATH_MAX 1024

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
static void build_norm_opts(const tp_project_atlas *a, const tp_pack_sprite_desc *sprites, int sprite_count,
                            tp_export_anim_in *anims, tp_export_name_override *ovs, tp_normalize_opts *out) {
    tp_normalize_opts_defaults(out);
    for (int i = 0; i < a->animation_count; i++) {
        const tp_project_anim *pa = &a->animations[i];
        anims[i].id = pa->id;
        anims[i].frames = (const char *const *)pa->frames;
        anims[i].frame_count = pa->frame_count;
        anims[i].fps = pa->fps;
        anims[i].playback = pa->playback;
        anims[i].flip_h = pa->flip_h;
        anims[i].flip_v = pa->flip_v;
    }
    out->animations = anims;
    out->animation_count = a->animation_count;

    int oc = 0;
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *ps = &a->sprites[i];
        if (!ps->rename || ps->rename[0] == '\0') {
            continue;
        }
        for (int d = 0; d < sprite_count; d++) {
            char key[TP_RUN_PATH_MAX];
            tp_sprite_export_key(sprites[d].name, key, sizeof key);
            if (strcmp(key, ps->name) == 0) {
                ovs[oc].raw_name = sprites[d].name;
                ovs[oc].final_name = ps->rename;
                oc++;
                break;
            }
        }
    }
    out->overrides = ovs;
    out->override_count = oc;
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

/* Fills rt->written_files from the target's writer output (two passes: count then
 * fill). Returns TP_STATUS_OOM on allocation failure. */
static tp_status collect_written_files(const tp_exporter *exp, const tp_export_prepared *prep, const char *out_base,
                                       tp_arena *arena, tp_export_report_target *rt) {
    file_collect c = {.arena = arena};
    list_target_outputs(exp, prep, out_base, file_count_sink, &c);
    int n = c.count;
    if (n <= 0) {
        rt->written_files = NULL;
        rt->written_file_count = 0;
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
    rt->written_files = arr;
    rt->written_file_count = (f.count < n) ? f.count : n;
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
                           int *out_pack_runs, tp_export_report *report, tp_error *err) {
    if (report) {
        memset(report, 0, sizeof *report);
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
    if (a->sprite_count > 0) {
        ovs = (tp_export_name_override *)tp_arena_alloc(arena, (size_t)a->sprite_count * sizeof(tp_export_name_override));
        if (!ovs) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (rename opts)");
        }
    }
    tp_normalize_opts nopts;
    build_norm_opts(a, sprites, sprite_count, anims, ovs, &nopts);

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

        int nbefore = notices ? notices->count : 0;
        if (rt) {
            rt->notice_begin = nbefore;
        }
        st = exp->write(&groups[target_group[t]].prep, &exp->caps, out_bases[t], notices, err);
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
            tp_status cst = collect_written_files(exp, &groups[target_group[t]].prep, out_bases[t], arena, rt);
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
