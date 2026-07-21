#ifndef TP_CORE_SRC_TP_SESSION_LAYOUT_H
#define TP_CORE_SRC_TP_SESSION_LAYOUT_H

#include "tp_session_internal.h"

/* struct tp_session embeds the platform lock by value, so the session family's
 * layout header carries the lock type. Only tp_session.c and tp_session_snapshot.c
 * read the layout; the other includers keep using tp_session as an opaque handle. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct tp_project_lease tp_project_lease;
typedef struct tp_session_owned_job tp_session_owned_job;

#define TP_SESSION_EVENT_CAPACITY 64U

/* Fixed FIFO cap on session-side visible-History markers (Save checkpoints /
 * runtime refreshes). Bounds the memory unconditionally; markers are also evicted
 * when their anchoring edit record leaves the window (tp_session.c). */
#define TP_SESSION_HISTORY_MARKER_CAP 64U

/* One non-undoable visible-History row the session owns (the model's undo stack
 * owns the edit rows). `anchor_pos` is the model history cursor (undoable-record
 * count) captured when the marker was created; it interleaves the marker into the
 * edit spine and lets redo-branch/FIFO eviction drop markers with their edits. */
typedef struct tp_session_history_marker {
    tp_session_history_kind kind;   /* SAVE_CHECKPOINT or RUNTIME_REFRESH */
    int anchor_pos;                 /* edit cursor at creation */
    int64_t revision;               /* model revision at creation (never advanced) */
    tp_id128 state_identity;        /* checkpoint: semantic identity; nil for refresh */
    char *path;                     /* checkpoint: owned canonical path; NULL for refresh */
} tp_session_history_marker;

/* Serialized single-writer session layout, shared by the writer TU and the
 * snapshot TU which samples committed fields under the gate. It stays private
 * to the session family: no frontend or protocol adapter includes this header. */
struct tp_session {
#if defined(_WIN32)
    SRWLOCK gate;
#else
    pthread_mutex_t gate;
#endif
    tp_model *model;
    tp_recovery_live *recovery_live;
    tp_project_lease *project_lease;
    tp_session_owned_job *active_job;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    tp_id128 recovery_token;
    bool has_saved_file_fingerprint;
    bool has_recovery_token;
    bool recovery_healthy;
    bool recovery_required;
    bool file_durability_uncertain;
    bool discarded;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    tp_session_event events[TP_SESSION_EVENT_CAPACITY];
    size_t event_count;
    size_t event_start;
    /* Visible-History markers, compacted oldest-first (index 0 is the oldest). */
    tp_session_history_marker markers[TP_SESSION_HISTORY_MARKER_CAP];
    size_t marker_count;
};

#endif /* TP_CORE_SRC_TP_SESSION_LAYOUT_H */
