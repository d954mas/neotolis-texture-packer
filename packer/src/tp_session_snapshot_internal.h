#ifndef TP_SESSION_SNAPSHOT_INTERNAL_H
#define TP_SESSION_SNAPSHOT_INTERNAL_H

#include "tp_core/tp_session_snapshot_query.h"

typedef struct tp_project_generation tp_project_generation;
typedef struct tp_session tp_session;

typedef struct tp_snapshot_atlas_storage {
    tp_snapshot_atlas dto;
    tp_snapshot_source *sources;
    tp_snapshot_sprite *sprites;
    tp_snapshot_animation *animations;
    tp_snapshot_frame *frames;
    int *frame_offsets;
    tp_snapshot_target *targets;
} tp_snapshot_atlas_storage;

struct tp_session_snapshot {
    tp_project_generation *generation;
    const tp_project *project;
    tp_snapshot_atlas_storage *atlases;
    int atlas_count;
    int64_t revision;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    bool dirty;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    bool has_saved_file_fingerprint;
};

/* Runs on the session owner thread; the entry point that reaches it has already
 * asserted ownership. The capture fixes the observation cut -- the exact
 * immutable generation plus the committed scalars -- but does not materialize
 * DTO arrays. */
tp_status tp_session_snapshot__capture(
    const tp_session *session, tp_session_snapshot **out, tp_error *err);
/* Materializes a captured snapshot after the cut, so a later commit cannot
 * change what it publishes. On failure the capture is destroyed; on success
 * ownership remains with the caller. */
tp_status tp_session_snapshot__materialize_captured(
    tp_session_snapshot *snapshot, tp_error *err);
/* Rebuilds only the snapshot half of the borrowed live view when its committed
 * cut changed. On failure the previous view remains intact. */
tp_status tp_session_view__refresh_snapshot(
    tp_session *session, tp_error *err);

#endif /* TP_SESSION_SNAPSHOT_INTERNAL_H */
