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

tp_status tp_export_run(const tp_project *project, int atlas_index, const tp_pack_sprite_desc *sprites,
                        int sprite_count, const char *work_dir, tp_arena *arena, tp_export_notices *notices,
                        int *out_pack_runs, tp_error *err) {
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
    if (!groups || !target_group) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_export_run: OOM (groups)");
    }
    int group_count = 0;

    /* Phase 1: resolve exporters, compute effective settings, pack once per
     * distinct effective-settings signature (SUMMARY.md §5h shared run). */
    for (int t = 0; t < a->target_count; t++) {
        const tp_project_target *tg = &a->targets[t];
        target_group[t] = -1;
        if (!tg->enabled) {
            continue;
        }
        const tp_exporter *exp = tp_exporter_find(tg->exporter_id);
        if (!exp) {
            return unknown_exporter(tg->exporter_id, err);
        }
        tp_pack_settings eff;
        st = tp_export_effective_settings(&base, &exp->caps, &eff);
        if (st != TP_STATUS_OK) {
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
                return st;
            }
            st = tp_normalize(groups[g].result, &nopts, arena, &groups[g].prep, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
        target_group[t] = g;
    }

    /* Phase 2: each enabled target writes its files from its group's prepared. */
    for (int t = 0; t < a->target_count; t++) {
        if (target_group[t] < 0) {
            continue;
        }
        const tp_project_target *tg = &a->targets[t];
        const tp_exporter *exp = tp_exporter_find(tg->exporter_id);
        char out_base[TP_RUN_PATH_MAX];
        st = tp_project_resolve_path(project, tg->out_path, out_base, sizeof out_base);
        if (st != TP_STATUS_OK) {
            return tp_error_set(err, st, "tp_export_run: cannot resolve out_path '%s'", tg->out_path);
        }
        st = exp->write(&groups[target_group[t]].prep, &exp->caps, out_base, notices, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
    }

    if (out_pack_runs) {
        *out_pack_runs = group_count;
    }
    return TP_STATUS_OK;
}
