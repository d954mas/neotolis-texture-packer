#ifndef TP_CORE_TP_SESSION_SNAPSHOT_QUERY_H
#define TP_CORE_TP_SESSION_SNAPSHOT_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_session_snapshot tp_session_snapshot;

/* Immutable identity of the inputs consumed by a derived job. Both zeroes are
 * valid generations; freshness is exact value equality, never a sentinel test. */
typedef struct tp_session_input_token {
    uint64_t model_generation;
    uint64_t source_generation;
} tp_session_input_token;
bool tp_session_input_token_equal(tp_session_input_token left,
                                  tp_session_input_token right);

/* Stable state-notice vocabulary for recovery durability. `notice_id` in the
 * DTO below always points at this program-lifetime string; consumers surface it
 * only while `degraded` is true. */
#define TP_SESSION_NOTICE_RECOVERY_DEGRADED "recovery_degraded"

typedef struct tp_session_recovery_health {
    const char *notice_id;
    bool available;
    bool degraded;
    tp_status first_cause;
    bool has_last_durable_revision;
    int64_t last_durable_revision;
    bool has_last_durable_time;
    int64_t last_durable_time;
    uint64_t generation;
} tp_session_recovery_health;

/* Immutable value DTOs owned by a tp_session_snapshot. Strings remain valid
 * only while the owner keeps that snapshot pinned. No project/model pointer
 * crosses this boundary. */
typedef struct tp_snapshot_atlas {
    tp_id128 id;
    const char *name;
    int max_size;
    int padding;
    int margin;
    int extrude;
    int alpha_threshold;
    int max_vertices;
    int shape;
    bool allow_transform;
    bool power_of_two;
    float pixels_per_unit;
    int source_count;
    int sprite_count;
    int animation_count;
    int target_count;
} tp_snapshot_atlas;

typedef enum tp_snapshot_source_kind {
    TP_SNAPSHOT_SOURCE_FOLDER = 0,
    TP_SNAPSHOT_SOURCE_FILE = 1
} tp_snapshot_source_kind;

typedef struct tp_snapshot_source {
    tp_id128 id;
    tp_snapshot_source_kind kind;
    const char *path;
} tp_snapshot_source;

typedef struct tp_snapshot_sprite {
    tp_id128 id;
    tp_id128 source_id;
    const char *source_key;
    const char *name;
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4];
    const char *rename;
    int16_t override_shape;
    int16_t override_allow_rotate;
    int16_t override_max_vertices;
    int16_t override_margin;
    int16_t override_extrude;
} tp_snapshot_sprite;

typedef struct tp_snapshot_frame {
    tp_id128 sprite_id;
    tp_id128 source_id;
    const char *source_key;
    const char *name;
} tp_snapshot_frame;

typedef struct tp_snapshot_animation {
    tp_id128 id;
    const char *name;
    float fps;
    int playback;
    bool flip_h;
    bool flip_v;
    int frame_count;
} tp_snapshot_animation;

typedef struct tp_snapshot_target {
    tp_id128 id;
    const char *exporter_id;
    const char *out_path;
    bool enabled;
} tp_snapshot_target;

/* Borrowed immutable accessors only. Snapshot/session creation, destruction,
 * mutation preview, admission, commands, and jobs deliberately live elsewhere. */
int64_t tp_session_snapshot_revision(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_model_generation(
    const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_admission_sequence(
    const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_source_generation(
    const tp_session_snapshot *snapshot);
tp_session_input_token tp_session_snapshot_input_token(
    const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_event_sequence(
    const tp_session_snapshot *snapshot);
bool tp_session_snapshot_dirty(const tp_session_snapshot *snapshot);
/* Recovery health is deliberately NOT a snapshot field. It is live session
 * state that changes without a project mutation, so a pinned snapshot would
 * hand out a stale copy. tp_session_view recovery health (fresh at the
 * observation cut) and tp_session_recovery_health_query are the only sources. */
tp_session_identity tp_session_snapshot_identity(
    const tp_session_snapshot *snapshot);
const char *tp_session_snapshot_canonical_path(
    const tp_session_snapshot *snapshot);
int tp_session_snapshot_project_schema_version(
    const tp_session_snapshot *snapshot);
const char *tp_session_snapshot_project_dir(
    const tp_session_snapshot *snapshot);
bool tp_session_snapshot_saved_file_fingerprint(
    const tp_session_snapshot *snapshot, tp_id128 *out_fingerprint);
int tp_session_snapshot_atlas_count(const tp_session_snapshot *snapshot);
const tp_snapshot_atlas *tp_session_snapshot_atlas_at(
    const tp_session_snapshot *snapshot, int index);
const tp_snapshot_atlas *tp_session_snapshot_atlas_by_id(
    const tp_session_snapshot *snapshot, tp_id128 id);
const tp_snapshot_source *tp_session_snapshot_source_at(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, int index);
tp_status tp_session_snapshot_source_resolved_at(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    const tp_snapshot_source **out_source, char *out_path, size_t capacity,
    tp_error *err);
const tp_snapshot_source *tp_session_snapshot_source_by_id(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 source_id);
const tp_snapshot_sprite *tp_session_snapshot_sprite_at(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, int index);
const tp_snapshot_sprite *tp_session_snapshot_sprite_at_index(
    const tp_session_snapshot *snapshot, int atlas_index, int sprite_index);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_key(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 source_id, const char *source_key);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_id(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 sprite_id);
const tp_snapshot_animation *tp_session_snapshot_animation_at(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, int index);
const tp_snapshot_animation *tp_session_snapshot_animation_by_id(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id);
const tp_snapshot_frame *tp_session_snapshot_animation_frame_at(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, int index);
const tp_snapshot_frame *tp_session_snapshot_animation_frames(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, int *out_count);
const tp_snapshot_target *tp_session_snapshot_target_at(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, int index);
const tp_snapshot_target *tp_session_snapshot_target_by_id(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id);
tp_status tp_session_snapshot_resolve_path(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 source_id, char *out, size_t capacity, tp_error *err);
tp_status tp_session_snapshot_resolve_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_scope,
    tp_selector_kind want, const char *selector, tp_selector_result *out,
    tp_selector_candidates *candidates, tp_error *err);
tp_status tp_session_snapshot_resolve_sprite_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, tp_selector_result *out, tp_id128 *out_source_id,
    char *out_source_key, size_t source_key_capacity,
    tp_selector_candidates *candidates, tp_error *err);
bool tp_session_snapshot_target_out_path_shared(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, const char *out_path);
tp_status tp_session_snapshot_next_atlas_defaults(
    const tp_session_snapshot *snapshot, char *name, size_t name_cap,
    char *out_path, size_t out_path_cap, const char **exporter_id,
    bool *target_enabled, tp_error *err);
tp_status tp_session_snapshot_next_animation_name(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, const char *base,
    char *name, size_t name_cap, tp_error *err);
tp_status tp_session_snapshot_target_defaults(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char **exporter_id, char *out_path, size_t out_path_cap,
    bool *enabled, tp_error *err);
tp_status tp_session_snapshot_resolve_frame(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, const char *selector, int *out_index,
    tp_error *err);
tp_status tp_session_snapshot_resolve_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, const tp_snapshot_target **out, tp_error *err);
tp_status tp_session_snapshot_serialize(
    const tp_session_snapshot *snapshot, char **out, size_t *out_len,
    tp_error *err);
tp_id128 tp_session_snapshot_semantic_identity(
    const tp_session_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SESSION_SNAPSHOT_QUERY_H */
