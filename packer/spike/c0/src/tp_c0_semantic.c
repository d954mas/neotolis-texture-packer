#include "tp_c0/tp_c0_semantic.h"

#include <stddef.h>

/* Persistent fields that feed semantic-state identity. Structural IDs are the
 * identity anchors; the packing knobs mirror atlas.settings.set; the sprite rows
 * mirror the sparse override record + logical name; anim `frames` is the sole
 * order-semantic collection. Mutable-name fields (atlas.name, animation.id) are
 * semantic content (an atlas.rename is an Undoable operation) even though the
 * ENTITY is ID-addressed. */
static const tp_c0_semantic_field k_semantic[] = {
    {"atlas", "atlas_id", false},
    {"atlas", "name", false},
    {"atlas", "max_size", false},
    {"atlas", "padding", false},
    {"atlas", "margin", false},
    {"atlas", "extrude", false},
    {"atlas", "alpha_threshold", false},
    {"atlas", "max_vertices", false},
    {"atlas", "shape", false},
    {"atlas", "allow_transform", false},
    {"atlas", "power_of_two", false},
    {"atlas", "pixels_per_unit", false},
    {"atlas", "sources", false},    /* collection keyed by source_id, order-normalized */
    {"atlas", "sprites", false},    /* sparse overrides keyed by sprite_id, order-normalized */
    {"atlas", "animations", false}, /* keyed by anim_id, order-normalized */
    {"atlas", "targets", false},    /* keyed by target_id, order-normalized */

    {"source", "source_id", false},
    {"source", "key", false}, /* normalized source-local root (C0-01 tp_c0_srckey) */

    {"sprite", "sprite_id", false}, /* derived from source_id + normalized key (§5.2) */
    {"sprite", "origin", false},
    {"sprite", "slice9", false},
    {"sprite", "rename", false}, /* logical/export name -> sprite.name.set */
    {"sprite", "ov_shape", false},
    {"sprite", "ov_allow_rotate", false},
    {"sprite", "ov_max_vertices", false},
    {"sprite", "ov_margin", false},
    {"sprite", "ov_extrude", false},

    {"animation", "anim_id", false},
    {"animation", "id", false},
    {"animation", "fps", false},
    {"animation", "playback", false},
    {"animation", "flip_h", false},
    {"animation", "flip_v", false},
    {"animation", "frames", true}, /* ORDER IS SEMANTIC: playback order */

    {"target", "target_id", false},
    {"target", "exporter_id", false},
    {"target", "out_path", false},
    {"target", "enabled", false},
};

/* Runtime state that never enters identity, dirty, or Undo. */
static const char *const k_excluded[] = {
    "revision_counter",         /* monotonic; dirty is NOT derived from it (§8) */
    "dirty_flag",               /* the comparison RESULT, not an input */
    "undo_redo_history",        /* session-local; survives ownership, resets on reopen (§9.3) */
    "redo_branch",              /* discarded by a new transaction after Undo */
    "saved_baseline_snapshot",  /* the comparison anchor, not content */
    "session_id",               /* runtime session/ownership identity (§16) */
    "ownership_authority_state",/* live claim / authority machine (§19, C0-03) */
    "external_controller_bind", /* one-controller binding (§18) */
    "pack_result",              /* derived job output (§10) */
    "pack_input_hash",          /* freshness derivation, not model state (§10.2) */
    "preview_hash",             /* freshness derivation (§10.2, §59 item 21) */
    "preview_freshness",        /* derived from hashes (§59 item 22) */
    "source_runtime_status",    /* missing/error runtime state (§11.4, §59 item 32) */
    "source_watchers",          /* watcher registrations (§11.2, §59 item 31) */
    "source_file_mtime_bytes",  /* change/decode-cache input only (§59 item 29) */
    "decoded_source_pixels",    /* decode cache; not retained in identity (§59 item 27) */
    "thumbnail_cache",          /* CPU/GPU LRU (§59 item 26) */
    "gui_view_state",           /* selection/window/focus/zoom (§17) */
    "gui_model_version_counter",/* apps/gui local s_model_ver, not a shared revision */
    "project_file_path",        /* identity KEY (§5.1), not semantic CONTENT */
    "schema_version",           /* serialization envelope, normalized away by migration */
};

int tp_c0_semantic_field_count(void) { return (int)(sizeof k_semantic / sizeof k_semantic[0]); }

const tp_c0_semantic_field *tp_c0_semantic_field_at(int i) {
    if (i < 0 || i >= tp_c0_semantic_field_count()) {
        return NULL;
    }
    return &k_semantic[i];
}

int tp_c0_runtime_excluded_count(void) { return (int)(sizeof k_excluded / sizeof k_excluded[0]); }

const char *tp_c0_runtime_excluded_at(int i) {
    if (i < 0 || i >= tp_c0_runtime_excluded_count()) {
        return NULL;
    }
    return k_excluded[i];
}
