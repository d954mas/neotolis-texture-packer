#include "gui_session_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_session.h"

static int fail(const char *step, tp_status status, const tp_error *err) {
    (void)fprintf(stderr, "client parity %s failed: %s (%s)\n", step,
                  tp_status_id(status), err ? err->msg : "");
    return 1;
}

static int deterministic_fill(void *ctx, uint8_t *dst, size_t count) {
    uint8_t *next = (uint8_t *)ctx;
    for (size_t i = 0U; i < count; ++i) {
        dst[i] = (*next)++;
    }
    return (int)count;
}

static const tp_snapshot_atlas *find_atlas(const tp_session_snapshot *snapshot,
                                            const char *name) {
    const int count = tp_session_snapshot_atlas_count(snapshot);
    for (int i = 0; i < count; ++i) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && strcmp(atlas->name, name) == 0) {
            return atlas;
        }
    }
    return NULL;
}

static const tp_snapshot_source *first_source(
    const tp_session_snapshot *snapshot, const tp_snapshot_atlas *atlas) {
    return atlas && atlas->source_count > 0
               ? tp_session_snapshot_source_at(snapshot, atlas->id, 0)
               : NULL;
}

static const tp_snapshot_animation *find_animation(
    const tp_session_snapshot *snapshot, const tp_snapshot_atlas *atlas,
    const char *name) {
    if (!atlas) {
        return NULL;
    }
    for (int i = 0; i < atlas->animation_count; ++i) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(snapshot, atlas->id, i);
        if (animation && strcmp(animation->name, name) == 0) {
            return animation;
        }
    }
    return NULL;
}

static const tp_snapshot_target *find_target(
    const tp_session_snapshot *snapshot, const tp_snapshot_atlas *atlas,
    const char *exporter_id) {
    if (!atlas) {
        return NULL;
    }
    for (int i = 0; i < atlas->target_count; ++i) {
        const tp_snapshot_target *target =
            tp_session_snapshot_target_at(snapshot, atlas->id, i);
        if (target && strcmp(target->exporter_id, exporter_id) == 0) {
            return target;
        }
    }
    return NULL;
}

static int64_t revision_of(tp_session *session, tp_error *err) {
    tp_session_snapshot *snapshot = NULL;
    if (tp_session_snapshot_create(session, &snapshot, err) != TP_STATUS_OK) {
        return -1;
    }
    const int64_t revision = tp_session_snapshot_revision(snapshot);
    tp_session_snapshot_destroy(snapshot);
    return revision;
}

static const char *next_txn(void) {
    static unsigned int next = 1U;
    static char id[33];
    (void)snprintf(id, sizeof id, "%032x", next++);
    return id;
}

static int save_replay(tp_session *session, const char *out_path,
                       tp_error *err) {
    tp_session_save_result result;
    memset(&result, 0, sizeof result);
    const tp_status status = tp_session_save_as(session, out_path, &result, err);
    return status == TP_STATUS_OK ? 0 : fail("save", status, err);
}

static int seed_project(const char *path) {
    uint8_t seed = 0x31U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {0};
    tp_session *session = NULL;
    tp_status status = tp_session_create_default_project(&rng, &session, &err);
    if (status != TP_STATUS_OK) {
        return fail("seed", status, &err);
    }
    tp_session_save_result result;
    memset(&result, 0, sizeof result);
    status = tp_session_save_new(session, path, &result, &err);
    tp_session_destroy(session);
    return status == TP_STATUS_OK ? 0 : fail("seed save", status, &err);
}

typedef struct harvest_ids {
    tp_id128 atlas_id;
    tp_id128 source_id;
    tp_id128 animation_id;
    tp_id128 target_id;
    tp_snapshot_source_kind source_kind;
    char source_path[4096];
    char target_exporter[256];
    char target_out_path[4096];
    bool target_enabled;
} harvest_ids;

static int harvest(const char *family, const char *path, harvest_ids *out) {
    uint8_t seed = 0x71U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {0};
    tp_session *session = NULL;
    tp_status status = tp_session_open(path, &rng, &session, &err);
    if (status != TP_STATUS_OK) {
        return fail("harvest open", status, &err);
    }
    tp_session_snapshot *snapshot = NULL;
    status = tp_session_snapshot_create(session, &snapshot, &err);
    if (status != TP_STATUS_OK) {
        tp_session_destroy(session);
        return fail("harvest snapshot", status, &err);
    }
    const char *atlas_name = strcmp(family, "atlas") == 0 ? "golden" : "atlas1";
    const tp_snapshot_atlas *atlas = find_atlas(snapshot, atlas_name);
    if (!atlas) {
        tp_session_snapshot_destroy(snapshot);
        tp_session_destroy(session);
        (void)fprintf(stderr, "client parity harvest: atlas '%s' absent\n", atlas_name);
        return 1;
    }
    out->atlas_id = atlas->id;
    if (strcmp(family, "source") == 0) {
        const tp_snapshot_source *source = first_source(snapshot, atlas);
        if (!source || snprintf(out->source_path, sizeof out->source_path, "%s",
                                source->path) < 0) {
            tp_session_snapshot_destroy(snapshot);
            tp_session_destroy(session);
            return 1;
        }
        out->source_id = source->id;
        out->source_kind = source->kind;
    } else if (strcmp(family, "animation") == 0) {
        const tp_snapshot_animation *animation =
            find_animation(snapshot, atlas, "stroll");
        if (!animation) {
            tp_session_snapshot_destroy(snapshot);
            tp_session_destroy(session);
            return 1;
        }
        out->animation_id = animation->id;
    } else if (strcmp(family, "target") == 0 ||
               strcmp(family, "atlas") == 0) {
        const char *exporter = strcmp(family, "target") == 0
                                   ? "defold"
                                   : "json-neotolis";
        const tp_snapshot_target *target = find_target(snapshot, atlas, exporter);
        if (!target) {
            tp_session_snapshot_destroy(snapshot);
            tp_session_destroy(session);
            return 1;
        }
        out->target_id = target->id;
        out->target_enabled = target->enabled;
        (void)snprintf(out->target_exporter, sizeof out->target_exporter,
                       "%s", target->exporter_id);
        (void)snprintf(out->target_out_path, sizeof out->target_out_path,
                       "%s", target->out_path);
    }
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    return 0;
}

static int open_base(const char *path, tp_session **out, tp_error *err) {
    uint8_t seed = 0x91U;
    const tp_rng rng = {deterministic_fill, &seed};
    const tp_status status = tp_session_open(path, &rng, out, err);
    return status == TP_STATUS_OK ? 0 : fail("base open", status, err);
}

static int base_atlas(tp_session *session, tp_id128 *atlas_id,
                      tp_error *err) {
    tp_session_snapshot *snapshot = NULL;
    tp_status status = tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return fail("base snapshot", status, err);
    }
    const tp_snapshot_atlas *atlas = find_atlas(snapshot, "atlas1");
    if (!atlas) {
        tp_session_snapshot_destroy(snapshot);
        return 1;
    }
    *atlas_id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return 0;
}

static int replay_atlas(tp_session *session, const harvest_ids *ids,
                        tp_error *err) {
    tp_status status = gui_session_create_atlas(
        session, ids->atlas_id, ids->target_id, revision_of(session, err),
        "two", ids->target_exporter, ids->target_out_path,
        ids->target_enabled, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("atlas create", status, err);
    status = gui_session_rename_atlas(session, ids->atlas_id,
                                      revision_of(session, err), "golden",
                                      next_txn(), err);
    if (status != TP_STATUS_OK) return fail("atlas rename", status, err);
    tp_op_atlas_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_AF_PADDING | TP_AF_MARGIN;
    settings.padding = 4;
    settings.margin = 2;
    status = gui_session_set_atlas_settings(session, ids->atlas_id,
                                            revision_of(session, err),
                                            &settings, next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("atlas settings", status, err);
}

static int replay_source(tp_session *session, const harvest_ids *ids,
                         tp_error *err) {
    tp_id128 atlas_id;
    if (base_atlas(session, &atlas_id, err) != 0) return 1;
    const char *paths[1] = {ids->source_path};
    const tp_status status = gui_session_add_sources(
        session, atlas_id, &ids->source_id, paths, 1, ids->source_kind,
        revision_of(session, err), next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("source add", status, err);
}

static int resolve_sprite(tp_session *session, tp_id128 atlas_id,
                          const char *selector, tp_id128 *source_id,
                          char *source_key, size_t source_key_capacity,
                          tp_error *err) {
    tp_session_snapshot *snapshot = NULL;
    tp_status status = tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) return fail("sprite snapshot", status, err);
    tp_selector_result result;
    tp_selector_candidates candidates;
    memset(&result, 0, sizeof result);
    memset(&candidates, 0, sizeof candidates);
    status = tp_session_snapshot_resolve_sprite_selector(
        snapshot, atlas_id, selector, &result, source_id, source_key,
        source_key_capacity, &candidates, err);
    tp_selector_candidates_free(&candidates);
    tp_session_snapshot_destroy(snapshot);
    return status == TP_STATUS_OK ? 0 : fail("sprite resolve", status, err);
}

static int replay_sprite(tp_session *session, tp_error *err) {
    tp_id128 atlas_id;
    tp_id128 source_id;
    char source_key[4096];
    if (base_atlas(session, &atlas_id, err) != 0 ||
        resolve_sprite(session, atlas_id, "hero", &source_id, source_key,
                       sizeof source_key, err) != 0) return 1;
    tp_status status = gui_session_set_sprite_name(
        session, atlas_id, source_id, source_key, revision_of(session, err),
        "hero_final", next_txn(), err);
    if (status != TP_STATUS_OK) return fail("sprite name", status, err);
    tp_op_sprite_set settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_SPF_ALL;
    settings.origin_x = 0.25F;
    settings.origin_y = 0.75F;
    settings.slice9[0] = 1;
    settings.slice9[1] = 2;
    settings.slice9[2] = 3;
    settings.slice9[3] = 4;
    settings.ov_shape = 0;
    settings.ov_allow_rotate = 0;
    settings.ov_max_vertices = 6;
    settings.ov_margin = 5;
    settings.ov_extrude = 2;
    status = gui_session_set_sprite_override(
        session, atlas_id, source_id, source_key, revision_of(session, err),
        &settings, next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("sprite settings", status, err);
}

static int replay_sprite_clear(tp_session *session, tp_error *err) {
    tp_id128 atlas_id;
    tp_id128 source_id;
    char source_key[4096];
    if (base_atlas(session, &atlas_id, err) != 0 ||
        resolve_sprite(session, atlas_id, "hero", &source_id, source_key,
                       sizeof source_key, err) != 0) return 1;
    tp_status status = gui_session_set_sprite_name(
        session, atlas_id, source_id, source_key, revision_of(session, err),
        NULL, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("sprite name clear", status, err);
    tp_op_sprite_set settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_SPF_ALL;
    settings.origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    settings.origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    settings.ov_shape = TP_PROJECT_OV_INHERIT;
    settings.ov_allow_rotate = TP_PROJECT_OV_INHERIT;
    settings.ov_max_vertices = TP_PROJECT_OV_INHERIT;
    settings.ov_margin = TP_PROJECT_OV_INHERIT;
    settings.ov_extrude = TP_PROJECT_OV_INHERIT;
    status = gui_session_set_sprite_override(
        session, atlas_id, source_id, source_key, revision_of(session, err),
        &settings, next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("sprite clear", status, err);
}

static int replay_animation(tp_session *session, const harvest_ids *ids,
                            tp_error *err) {
    tp_id128 atlas_id;
    tp_id128 hero_source;
    tp_id128 coin_source;
    char hero_key[4096];
    char coin_key[4096];
    if (base_atlas(session, &atlas_id, err) != 0 ||
        resolve_sprite(session, atlas_id, "hero", &hero_source, hero_key,
                       sizeof hero_key, err) != 0 ||
        resolve_sprite(session, atlas_id, "coin", &coin_source, coin_key,
                       sizeof coin_key, err) != 0) return 1;
    tp_op_sprite_ref frames[2] = {
        {hero_source, hero_key}, {coin_source, coin_key}};
    tp_status status = gui_session_create_animation(
        session, atlas_id, ids->animation_id, revision_of(session, err),
        "walk", frames, 2, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("animation create", status, err);
    status = gui_session_add_animation_frames(
        session, atlas_id, ids->animation_id, revision_of(session, err),
        frames, 1, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("animation frame add", status, err);
    status = gui_session_move_animation_frame(
        session, atlas_id, ids->animation_id, revision_of(session, err), 0, 2,
        next_txn(), err);
    if (status != TP_STATUS_OK) return fail("animation frame move", status, err);
    status = gui_session_remove_animation_frame(
        session, atlas_id, ids->animation_id, revision_of(session, err), 1,
        next_txn(), err);
    if (status != TP_STATUS_OK) return fail("animation frame remove", status, err);
    tp_op_anim_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_ANF_ALL;
    settings.fps = 12.0F;
    settings.playback = 1;
    settings.flip_h = true;
    settings.flip_v = false;
    status = gui_session_set_animation_settings(
        session, atlas_id, ids->animation_id, revision_of(session, err),
        &settings, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("animation settings", status, err);
    status = gui_session_rename_animation(
        session, atlas_id, ids->animation_id, revision_of(session, err),
        "stroll", next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("animation rename", status, err);
}

static int replay_target(tp_session *session, const harvest_ids *ids,
                         tp_error *err) {
    tp_id128 atlas_id;
    if (base_atlas(session, &atlas_id, err) != 0) return 1;
    tp_status status = gui_session_create_target(
        session, atlas_id, ids->target_id, revision_of(session, err),
        "defold", "out/d", true, next_txn(), err);
    if (status != TP_STATUS_OK) return fail("target create", status, err);
    tp_op_target_set settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_TF_OUT_PATH | TP_TF_ENABLED;
    settings.out_path = "out/d2";
    settings.enabled = false;
    status = gui_session_set_target(session, atlas_id, ids->target_id,
                                    revision_of(session, err), &settings,
                                    next_txn(), err);
    return status == TP_STATUS_OK ? 0 : fail("target settings", status, err);
}

static int replay_remove(tp_session *session, const char *family,
                         tp_error *err) {
    tp_id128 atlas_id;
    if (base_atlas(session, &atlas_id, err) != 0) return 1;
    tp_session_snapshot *snapshot = NULL;
    tp_status status = tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) return fail("remove snapshot", status, err);
    if (strcmp(family, "atlas_remove") == 0) {
        const tp_snapshot_atlas *atlas = find_atlas(snapshot, "golden");
        if (!atlas) { tp_session_snapshot_destroy(snapshot); return 1; }
        const tp_id128 id = atlas->id;
        tp_session_snapshot_destroy(snapshot);
        status = gui_session_remove_atlas(session, id, revision_of(session, err),
                                          next_txn(), err);
    } else if (strcmp(family, "source_remove") == 0) {
        const tp_snapshot_atlas *atlas = find_atlas(snapshot, "atlas1");
        const tp_snapshot_source *source = first_source(snapshot, atlas);
        if (!source) { tp_session_snapshot_destroy(snapshot); return 1; }
        const tp_id128 id = source->id;
        tp_session_snapshot_destroy(snapshot);
        status = gui_session_remove_source(session, atlas_id, id,
                                            revision_of(session, err),
                                            next_txn(), err);
    } else if (strcmp(family, "animation_remove") == 0) {
        const tp_snapshot_atlas *atlas = find_atlas(snapshot, "atlas1");
        const tp_snapshot_animation *animation =
            find_animation(snapshot, atlas, "stroll");
        if (!animation) { tp_session_snapshot_destroy(snapshot); return 1; }
        const tp_id128 id = animation->id;
        tp_session_snapshot_destroy(snapshot);
        status = gui_session_remove_animation(session, atlas_id, id,
                                               revision_of(session, err),
                                               next_txn(), err);
    } else {
        const tp_snapshot_atlas *atlas = find_atlas(snapshot, "atlas1");
        const tp_snapshot_target *target = find_target(snapshot, atlas, "defold");
        if (!target) { tp_session_snapshot_destroy(snapshot); return 1; }
        const tp_id128 id = target->id;
        tp_session_snapshot_destroy(snapshot);
        status = gui_session_remove_target(session, atlas_id, id,
                                            revision_of(session, err),
                                            next_txn(), err);
    }
    return status == TP_STATUS_OK ? 0 : fail("remove", status, err);
}

static int replay(const char *family, const char *base_path,
                  const char *harvest_path, const char *out_path) {
    harvest_ids ids;
    memset(&ids, 0, sizeof ids);
    if ((strcmp(family, "atlas") == 0 || strcmp(family, "source") == 0 ||
         strcmp(family, "animation") == 0 || strcmp(family, "target") == 0) &&
        harvest(family, harvest_path, &ids) != 0) {
        return 1;
    }
    tp_error err = {0};
    tp_session *session = NULL;
    if (open_base(base_path, &session, &err) != 0) return 1;
    int rc = 0;
    if (strcmp(family, "atlas") == 0) rc = replay_atlas(session, &ids, &err);
    else if (strcmp(family, "source") == 0) rc = replay_source(session, &ids, &err);
    else if (strcmp(family, "sprite") == 0) rc = replay_sprite(session, &err);
    else if (strcmp(family, "sprite_clear") == 0) rc = replay_sprite_clear(session, &err);
    else if (strcmp(family, "animation") == 0) rc = replay_animation(session, &ids, &err);
    else if (strcmp(family, "target") == 0) rc = replay_target(session, &ids, &err);
    else rc = replay_remove(session, family, &err);
    if (rc == 0) rc = save_replay(session, out_path, &err);
    tp_session_destroy(session);
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "seed") == 0) {
        return seed_project(argv[2]);
    }
    if (argc == 6 && strcmp(argv[1], "replay") == 0) {
        return replay(argv[2], argv[3], argv[4], argv[5]);
    }
    (void)fprintf(stderr,
                  "usage: client_parity_replay seed PATH | replay FAMILY BASE HARVEST OUT\n");
    return 2;
}
