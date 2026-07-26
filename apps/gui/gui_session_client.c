#include "gui_session_client.h"

#include <string.h>

#include "core/nt_assert.h"

#ifdef TP_ENABLE_TEST_SEAMS
static bool s_test_fail_next_observe;
#endif

static void destroy_observation_pair(
    tp_session_observation *latest,
    tp_session_observation *snapshot_owner) {
    if (latest && latest != snapshot_owner) {
        tp_session_observation_destroy(latest);
    }
    tp_session_observation_destroy(snapshot_owner);
}

static tp_status observe_core(
    tp_session *session,
    const tp_session_observation_token *after,
    tp_session_observation **out, tp_error *err) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_test_fail_next_observe) {
        s_test_fail_next_observe = false;
        if (out) {
            *out = NULL;
        }
        return tp_error_set(
            err, TP_STATUS_OOM,
            "GUI session observation test allocation failure");
    }
#endif
    return tp_session_observe(session, after, out, err);
}

static void reduce_observation(
    const gui_session_client *client,
    const tp_session_observation *observation,
    uint64_t instance_generation) {
    for (size_t index = 0U;
         index < client->reducer_count; ++index) {
        NT_ASSERT(client->reducers[index].reduce != NULL);
        client->reducers[index].reduce(
            client->reducers[index].context, observation,
            instance_generation);
    }
}

static void publish_observation(
    gui_session_client *client,
    tp_session_observation *next,
    uint64_t instance_generation) {
    NT_ASSERT(client != NULL);
    NT_ASSERT(next != NULL);
    NT_ASSERT(!client->frame_pinned);

    const tp_session_snapshot *next_snapshot =
        tp_session_observation_snapshot(next);
    tp_session_observation *old_latest = client->latest;
    tp_session_observation *old_snapshot_owner =
        client->snapshot_owner;

    /* Reducers see the complete batch before the new frame state becomes
     * externally readable through the client. Reducers are deterministic
     * non-owning state machines; fallible materialization happened in core. */
    reduce_observation(client, next, instance_generation);

    client->latest = next;
    client->observed =
        tp_session_observation_token_query(next);
    if (next_snapshot) {
        client->snapshot_owner = next;
        client->snapshot_lifetime_generation++;
        destroy_observation_pair(
            old_latest, old_snapshot_owner);
    } else {
        client->snapshot_owner = old_snapshot_owner;
        if (old_latest &&
            old_latest != old_snapshot_owner) {
            tp_session_observation_destroy(old_latest);
        }
    }
    client->observe_requested = false;
}

void gui_session_client_init(gui_session_client *client) {
    if (!client) {
        return;
    }
    memset(client, 0, sizeof *client);
}

tp_status gui_session_client_register_reducer(
    gui_session_client *client,
    gui_session_client_reducer_fn reduce,
    void *context, tp_error *err) {
    if (!client || !reduce) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session reducer registration requires client and reducer");
    }
    if (client->reducer_count >=
        GUI_SESSION_CLIENT_MAX_REDUCERS) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "GUI session reducer registry is full");
    }
    client->reducers[client->reducer_count++] =
        (gui_session_client_reducer){
            .reduce = reduce,
            .context = context,
        };
    return TP_STATUS_OK;
}

tp_status gui_session_client_attach(
    gui_session_client *client, tp_session *session,
    tp_error *err) {
    if (!client || !session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session attach requires client and session");
    }
    if (client->frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session attach is forbidden during a pinned frame");
    }

    tp_session_observation *initial = NULL;
    const tp_status status =
        observe_core(session, NULL, &initial, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!initial ||
        !tp_session_observation_snapshot(initial)) {
        tp_session_observation_destroy(initial);
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session attach did not produce a complete observation");
    }

    uint64_t next_generation =
        client->session_instance_generation + 1U;
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    publish_observation(
        client, initial, next_generation);
    client->session = session;
    client->session_instance_generation =
        next_generation;
    return TP_STATUS_OK;
}

tp_status gui_session_client_observe(
    gui_session_client *client, tp_error *err) {
    if (!client || !client->session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session observe requires an attached session");
    }
    if (client->frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session observation swap is forbidden during a pinned frame");
    }

    tp_session_observation *next = NULL;
    const tp_status status = observe_core(
        client->session, &client->observed, &next, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!next) {
        client->observe_requested = false;
        return TP_STATUS_OK;
    }
    publish_observation(
        client, next,
        client->session_instance_generation);
    return TP_STATUS_OK;
}

tp_status gui_session_client_resync(
    gui_session_client *client, tp_error *err) {
    if (!client || !client->session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session resync requires an attached session");
    }
    if (client->frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session resync is forbidden during a pinned frame");
    }
    tp_session_observation *next = NULL;
    const tp_status status =
        observe_core(client->session, NULL, &next, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!next ||
        !tp_session_observation_snapshot(next)) {
        tp_session_observation_destroy(next);
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session resync did not produce a complete observation");
    }
    publish_observation(
        client, next,
        client->session_instance_generation);
    return TP_STATUS_OK;
}

tp_status gui_session_client_frame_begin(
    gui_session_client *client, tp_error *err) {
    if (!client || !client->session ||
        !client->snapshot_owner) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session frame requires an attached observation");
    }
    if (client->frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session frame is already pinned");
    }
    const tp_status status =
        gui_session_client_observe(client, err);
    /* A failed refresh keeps and pins the last valid frame observation so
     * presentation remains coherent and retryable. */
    client->frame_pinned = true;
    return status;
}

void gui_session_client_frame_end(
    gui_session_client *client) {
    if (!client) {
        return;
    }
    NT_ASSERT(client->frame_pinned);
    client->frame_pinned = false;
}

tp_status gui_session_client_request_observe(
    gui_session_client *client, tp_error *err) {
    if (!client || !client->session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session observation request requires an attached session");
    }
    client->observe_requested = true;
    if (client->frame_pinned) {
        return TP_STATUS_OK;
    }
    return gui_session_client_observe(client, err);
}

void gui_session_client_detach(
    gui_session_client *client) {
    if (!client) {
        return;
    }
    NT_ASSERT(!client->frame_pinned);
    const bool had_snapshot =
        client->snapshot_owner != NULL;
    destroy_observation_pair(
        client->latest, client->snapshot_owner);
    client->session = NULL;
    client->latest = NULL;
    client->snapshot_owner = NULL;
    client->observed =
        (tp_session_observation_token){0};
    client->observe_requested = false;
    if (had_snapshot) {
        client->snapshot_lifetime_generation++;
    }
}

const tp_session_snapshot *gui_session_client_snapshot(
    const gui_session_client *client) {
    return client && client->snapshot_owner
               ? tp_session_observation_snapshot(
                     client->snapshot_owner)
               : NULL;
}

const tp_session_observation *
gui_session_client_observation(
    const gui_session_client *client) {
    return client ? client->latest : NULL;
}

tp_session_observation_token
gui_session_client_observed_token(
    const gui_session_client *client) {
    return client ? client->observed
                  : (tp_session_observation_token){0};
}

uint64_t gui_session_client_instance_generation(
    const gui_session_client *client) {
    return client
               ? client->session_instance_generation
               : 0U;
}

uint64_t
gui_session_client_snapshot_lifetime_generation(
    const gui_session_client *client) {
    return client
               ? client->snapshot_lifetime_generation
               : 0U;
}

uint64_t
gui_session_client_source_runtime_generation(
    const gui_session_client *client) {
    return client
               ? client->observed.source_runtime_generation
               : 0U;
}

bool gui_session_client_frame_is_pinned(
    const gui_session_client *client) {
    return client && client->frame_pinned;
}

#ifdef TP_ENABLE_TEST_SEAMS
void gui_session_client__test_fail_next_observe(void) {
    s_test_fail_next_observe = true;
}
#endif
