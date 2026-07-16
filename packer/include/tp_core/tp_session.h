#ifndef TP_CORE_TP_SESSION_H
#define TP_CORE_TP_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_session tp_session;
typedef struct tp_session_snapshot tp_session_snapshot;
typedef struct tp_txn_request tp_txn_request;
typedef struct tp_txn_result tp_txn_result;
typedef struct tp_project tp_project;
typedef struct tp_journal tp_journal;

/* Session calls are synchronous and serialized. They are intentionally
 * non-reentrant: side-effect/journal callbacks invoked during admission must not
 * call back into this same session. There is no actor thread or hidden mailbox. */

/* Immutable value DTOs owned by a tp_session_snapshot. Strings remain valid until
 * the snapshot is destroyed. No project/model pointer crosses this boundary. */
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

typedef enum tp_session_event_kind {
    TP_SESSION_EVENT_MODEL_COMMITTED = 1,
    TP_SESSION_EVENT_UNDONE = 2,
    TP_SESSION_EVENT_REDONE = 3,
    TP_SESSION_EVENT_SAVED = 4,
    TP_SESSION_EVENT_DISCARDED = 5,
    TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED = 6
} tp_session_event_kind;

typedef struct tp_session_event {
    uint64_t sequence;
    tp_session_event_kind kind;
    int64_t revision_before;
    int64_t revision_after;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    char transaction_id[33];
} tp_session_event;

typedef struct tp_session_save_result {
    bool saved;
    bool recovery_degraded;
    tp_status recovery_status;
    char target_path[TP_IDENTITY_PATH_MAX];
    tp_id128 file_fingerprint;
    tp_id128 recovery_token;
    bool has_recovery_token;
} tp_session_save_result;

/* Creates a synchronous single-writer session over a new default project. The
 * session is the sole owner of its model. `rng` is used for session/structural IDs. */
tp_status tp_session_create(const tp_rng *rng, tp_session **out, tp_error *err);
tp_status tp_session_create_default_project(const tp_rng *rng,
                                            tp_session **out,
                                            tp_error *err);
tp_status tp_session_open(const char *path, const tp_rng *rng,
                          tp_session **out, tp_error *err);

void tp_session_destroy(tp_session *session);

tp_status tp_session_apply(tp_session *session, const tp_txn_request *request,
                           tp_txn_result *result, tp_error *err);
tp_status tp_session_undo(tp_session *session, tp_error *err);
tp_status tp_session_redo(tp_session *session, tp_error *err);
tp_status tp_session_save(tp_session *session, tp_session_save_result *result,
                          tp_error *err);
tp_status tp_session_save_as(tp_session *session, const char *path,
                             tp_session_save_result *result, tp_error *err);
tp_status tp_session_discard(tp_session *session, tp_error *err);
tp_status tp_session_invalidate_sources(tp_session *session, tp_error *err);
/* Makes recovery acknowledgement mandatory for this live host. Call before
 * attaching recovery so setup/lock failures cannot open an unjournaled
 * mutation window. Session save/discard remain available for escape/cleanup. */
tp_status tp_session_require_recovery(tp_session *session, tp_error *err);
int64_t tp_session_revision(const tp_session *session);
bool tp_session_recovery_available(const tp_session *session);
bool tp_session_can_undo(const tp_session *session);
bool tp_session_can_redo(const tp_session *session);
int tp_session_undo_depth(const tp_session *session);
int tp_session_redo_depth(const tp_session *session);
uint64_t tp_session_event_sequence(const tp_session *session);

/* Returns committed events after `after_sequence`. If the bounded event window no
 * longer contains the requested sequence, returns zero events and asks the caller
 * to resync from an owned snapshot. */
tp_status tp_session_events_after(const tp_session *session, uint64_t after_sequence,
                                  tp_session_event *out, size_t capacity,
                                  size_t *out_count, bool *out_resync_required,
                                  tp_error *err);

tp_status tp_session_snapshot_create(const tp_session *session,
                                     tp_session_snapshot **out, tp_error *err);
/* Loads one immutable file-oriented snapshot without acquiring the exclusive
 * writer lease or promoting loader-synthesized legacy IDs. This is the read
 * boundary for one-shot inspect/validate/pack clients. Relative project paths
 * use the same canonical identity owner as tp_session_open(). */
tp_status tp_session_snapshot_load(const char *path,
                                   tp_session_snapshot **out, tp_error *err);
void tp_session_snapshot_destroy(tp_session_snapshot *snapshot);
int64_t tp_session_snapshot_revision(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_model_generation(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_admission_sequence(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_source_generation(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_event_sequence(const tp_session_snapshot *snapshot);
bool tp_session_snapshot_dirty(const tp_session_snapshot *snapshot);
bool tp_session_snapshot_recovery_available(const tp_session_snapshot *snapshot);
tp_session_identity tp_session_snapshot_identity(const tp_session_snapshot *snapshot);
/* Borrowed immutable path owned by `snapshot`; empty for an unsaved identity. */
const char *tp_session_snapshot_canonical_path(const tp_session_snapshot *snapshot);
int tp_session_snapshot_project_schema_version(const tp_session_snapshot *snapshot);
const char *tp_session_snapshot_project_dir(const tp_session_snapshot *snapshot);
/* Returns the exact bytes fingerprint captured by Open/last successful Save.
 * The value is session-owned authority copied into this immutable snapshot;
 * callers must not re-fingerprint the path to reconstruct the baseline. */
bool tp_session_snapshot_saved_file_fingerprint(
    const tp_session_snapshot *snapshot, tp_id128 *out_fingerprint);
int tp_session_snapshot_atlas_count(const tp_session_snapshot *snapshot);
const tp_snapshot_atlas *tp_session_snapshot_atlas_at(const tp_session_snapshot *snapshot,
                                                      int index);
const tp_snapshot_atlas *tp_session_snapshot_atlas_by_id(const tp_session_snapshot *snapshot,
                                                         tp_id128 id);
const tp_snapshot_source *tp_session_snapshot_source_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
/* Direct atlas/source-index query for linear snapshot consumers. Returns the
 * immutable source DTO and resolves its path without rescanning atlas/source
 * IDs. For valid indices, *out_source is returned even when path resolution
 * fails, allowing callers to report the unresolved source without a second
 * lookup. */
tp_status tp_session_snapshot_source_resolved_at(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    const tp_snapshot_source **out_source, char *out_path, size_t capacity,
    tp_error *err);
const tp_snapshot_source *tp_session_snapshot_source_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 source_id);
const tp_snapshot_sprite *tp_session_snapshot_sprite_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
/* Direct index query for consumers already iterating one atlas DTO. */
const tp_snapshot_sprite *tp_session_snapshot_sprite_at_index(
    const tp_session_snapshot *snapshot, int atlas_index, int sprite_index);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_key(const tp_session_snapshot *snapshot,
                                                            tp_id128 atlas_id, tp_id128 source_id,
                                                            const char *source_key);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 sprite_id);
const tp_snapshot_animation *tp_session_snapshot_animation_at(const tp_session_snapshot *snapshot,
                                                              tp_id128 atlas_id, int index);
const tp_snapshot_animation *tp_session_snapshot_animation_by_id(const tp_session_snapshot *snapshot,
                                                                 tp_id128 atlas_id, tp_id128 animation_id);
const tp_snapshot_frame *tp_session_snapshot_animation_frame_at(const tp_session_snapshot *snapshot,
                                                                tp_id128 atlas_id, tp_id128 animation_id,
                                                                int index);
const tp_snapshot_target *tp_session_snapshot_target_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
const tp_snapshot_target *tp_session_snapshot_target_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 target_id);
tp_status tp_session_snapshot_resolve_path(const tp_session_snapshot *snapshot,
                                           tp_id128 atlas_id, tp_id128 source_id,
                                           char *out, size_t capacity, tp_error *err);
/* Resolves one structural selector through the canonical selector owner without
 * exposing the snapshot's project. A non-nil atlas_scope limits child-entity
 * matching to that atlas before ambiguity is decided. */
tp_status tp_session_snapshot_resolve_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_scope,
    tp_selector_kind want, const char *selector, tp_selector_result *out,
    tp_selector_candidates *candidates, tp_error *err);
/* Resolve a runtime sprite selector within one atlas to its canonical persistent
 * address. The selector grammar is owned by tp_selector: sprite_<id>, sprite:<key>,
 * or source_<id>:<key>. A bare/export key is accepted as sprite:<key>, but multiple
 * matching sources return AMBIGUOUS_SELECTOR; the first match is never selected.
 * On success `out_source_key` receives the normalized, extension-preserving key. */
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
tp_status tp_session_snapshot_serialize(const tp_session_snapshot *snapshot,
                                        char **out, size_t *out_len,
                                        tp_error *err);
tp_id128 tp_session_snapshot_semantic_identity(
    const tp_session_snapshot *snapshot);
#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SESSION_H */
