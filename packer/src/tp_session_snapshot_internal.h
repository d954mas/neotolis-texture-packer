#ifndef TP_SESSION_SNAPSHOT_INTERNAL_H
#define TP_SESSION_SNAPSHOT_INTERNAL_H

#include "tp_core/tp_session.h"

typedef struct tp_project_generation tp_project_generation;

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
    bool recovery_healthy;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    bool has_saved_file_fingerprint;
};

#endif /* TP_SESSION_SNAPSHOT_INTERNAL_H */
