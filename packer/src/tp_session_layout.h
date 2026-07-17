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
    bool discarded;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    tp_session_event events[TP_SESSION_EVENT_CAPACITY];
    size_t event_count;
    size_t event_start;
};

#endif /* TP_CORE_SRC_TP_SESSION_LAYOUT_H */
