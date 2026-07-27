#include "gui_session_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"

#ifdef TP_ENABLE_TEST_SEAMS
static unsigned int s_test_observe_failures;
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
    if (s_test_observe_failures > 0U) {
        s_test_observe_failures--;
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

static void pending_clear(
    gui_session_pending_submit *pending) {
    if (!pending) {
        return;
    }
    memset(pending, 0, sizeof *pending);
}

static bool pending_occupied(
    const gui_session_pending_submit *pending) {
    return pending &&
           pending->terminal.transaction_id[0] != '\0';
}

static void submit_state_clear(
    gui_session_client *client) {
    NT_ASSERT(client != NULL);
    for (size_t index = 0U;
         index < GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++index) {
        pending_clear(&client->pending[index]);
    }
}

static void reconcile_submit_echoes(
    gui_session_client *client,
    const tp_session_observation *observation) {
    NT_ASSERT(client != NULL);
    NT_ASSERT(observation != NULL);
    const tp_session_snapshot *snapshot =
        tp_session_observation_snapshot(observation);
    const bool resync =
        tp_session_observation_resync_required(
            observation);
    const int64_t observed_revision =
        snapshot
            ? tp_session_snapshot_revision(snapshot)
            : -1;
    const size_t event_count =
        tp_session_observation_event_count(observation);
    for (size_t pending_index = 0U;
         pending_index <
         GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++pending_index) {
        gui_session_pending_submit *pending =
            &client->pending[pending_index];
        if (!pending_occupied(pending) ||
            pending->terminal.echo_state !=
                GUI_SESSION_SUBMIT_ECHO_PENDING) {
            continue;
        }
        bool observed =
            resync &&
            observed_revision >=
                pending->terminal.revision;
        for (size_t event_index = 0U;
             !observed && event_index < event_count;
             ++event_index) {
            const tp_session_event *event =
                tp_session_observation_event_at(
                    observation, event_index);
            observed =
                event &&
                event->kind ==
                    TP_SESSION_EVENT_MODEL_COMMITTED &&
                strcmp(
                    event->transaction_id,
                    pending->terminal.transaction_id) ==
                    0;
        }
        if (!observed) {
            continue;
        }
        pending->observation_status =
            TP_STATUS_OK;
        pending->terminal.echo_state =
            GUI_SESSION_SUBMIT_ECHO_OBSERVED;
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

    /* Exact own-submit acknowledgement is part of the same atomic reduction
     * cut and is visible to draft reducers processing this batch. */
    reconcile_submit_echoes(client, next);
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
}

static bool submit_identity_equal(
    gui_session_submit_identity left,
    gui_session_submit_identity right) {
    return tp_id128_eq(
               left.origin_view_id,
               right.origin_view_id) &&
           tp_id128_eq(
               left.draft_instance_id,
               right.draft_instance_id);
}

static bool transaction_id_is_canonical(
    const char *transaction_id) {
    if (!transaction_id ||
        strlen(transaction_id) != 32U) {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        const unsigned char byte =
            (unsigned char)transaction_id[index];
        if (!isdigit(byte) &&
            !(byte >= (unsigned char)'a' &&
              byte <= (unsigned char)'f')) {
            return false;
        }
    }
    return true;
}

static void transaction_id_format(
    tp_id128 id, char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < 16U; ++index) {
        out[index * 2U] =
            hex[id.bytes[index] >> 4U];
        out[index * 2U + 1U] =
            hex[id.bytes[index] & 0x0fU];
    }
    out[32] = '\0';
}

static tp_status submit_transaction_id(
    gui_session_client *client,
    const char *retained,
    char out[33], tp_error *err) {
    if (retained) {
        if (!transaction_id_is_canonical(retained)) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "GUI retained transaction ID must be 32 lowercase hex digits");
        }
        (void)snprintf(out, 33U, "%s", retained);
        return TP_STATUS_OK;
    }
    tp_id128 generated = tp_id128_nil();
    const tp_status status = tp_id128_generate(
        &client->transaction_rng, &generated, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    transaction_id_format(generated, out);
    return TP_STATUS_OK;
}

static gui_session_pending_submit *pending_find_id(
    gui_session_client *client,
    const char transaction_id[33]) {
    for (size_t index = 0U;
         index < GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++index) {
        gui_session_pending_submit *pending =
            &client->pending[index];
        if (pending_occupied(pending) &&
            strcmp(
                pending->terminal.transaction_id,
                transaction_id) == 0) {
            return pending;
        }
    }
    return NULL;
}

static tp_status pending_register_before_admission(
    gui_session_client *client,
    const char transaction_id[33],
    gui_session_submit_identity identity,
    gui_session_pending_submit **out,
    tp_error *err) {
    gui_session_pending_submit *pending =
        pending_find_id(client, transaction_id);
    if (pending) {
        if (!submit_identity_equal(
                pending->terminal.identity,
                identity)) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "GUI retained transaction identity does not match its original view and draft");
        }
        *out = pending;
        return TP_STATUS_OK;
    }

    for (size_t index = 0U;
         index < GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++index) {
        if (!pending_occupied(&client->pending[index])) {
            pending = &client->pending[index];
            break;
        }
    }
    if (!pending) {
        for (size_t index = 0U;
             index <
             GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
             ++index) {
            gui_session_pending_submit *candidate =
                &client->pending[index];
            if (candidate->terminal.echo_state !=
                    GUI_SESSION_SUBMIT_ECHO_PENDING) {
                pending = candidate;
                break;
            }
        }
    }
    if (!pending) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "GUI pending-submit receipt capacity is exhausted by unresolved submissions");
    }
    pending_clear(pending);
    (void)snprintf(
        pending->terminal.transaction_id,
        sizeof pending->terminal.transaction_id,
        "%s", transaction_id);
    pending->terminal.identity = identity;
    *out = pending;
    return TP_STATUS_OK;
}

/* Receipt postcondition: EVERY submit that resolved a transaction id answers
 * with a typed terminal (id + identity + status + the revision the caller can
 * still see). The GUI draft owner reads an EMPTY terminal transaction id as "the
 * session never saw this submit" and returns the draft to EDITING, so a silent
 * receipt-less rejection is the one answer that must never happen. */
static tp_status fail_submit_with_receipt(
    const gui_session_client *client,
    const gui_session_submit_request *request,
    const char transaction_id[33],
    gui_session_submit_result *out,
    tp_status status) {
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(client);
    out->terminal_status = status;
    (void)snprintf(
        out->terminal.transaction_id,
        sizeof out->terminal.transaction_id,
        "%s", transaction_id);
    out->terminal.identity = request->identity;
    out->terminal.status = status;
    out->terminal.revision =
        snapshot
            ? tp_session_snapshot_revision(snapshot)
            : request->expected_revision;
    return status;
}

static tp_status reject_submit(
    const gui_session_client *client,
    const gui_session_submit_request *request,
    const char transaction_id[33],
    gui_session_submit_result *out,
    const char *message,
    tp_error *err) {
    const tp_status status = tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT, "%s",
        message);
    return fail_submit_with_receipt(
        client, request, transaction_id, out, status);
}

static void pending_finish(
    gui_session_client *client,
    gui_session_pending_submit *pending,
    tp_status status,
    const tp_txn_result *transaction) {
    NT_ASSERT(client != NULL);
    NT_ASSERT(pending != NULL);
    NT_ASSERT(transaction != NULL);
    pending->terminal.status = status;
    pending->terminal.committed =
        transaction->committed;
    pending->terminal.no_change =
        transaction->no_change;
    pending->terminal.revision =
        transaction->revision;
    pending->terminal.echo_state =
        transaction->committed &&
                !transaction->no_change
            ? GUI_SESSION_SUBMIT_ECHO_PENDING
            : GUI_SESSION_SUBMIT_ECHO_NOT_APPLICABLE;
    pending->observation_status =
        TP_STATUS_OK;
}

void gui_session_client_init(gui_session_client *client) {
    if (!client) {
        return;
    }
    memset(client, 0, sizeof *client);
    client->transaction_rng = tp_rng_os();
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
    if (client->session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session attach is initial-empty only");
    }
    gui_session_client_prepared prepared = {0};
    const tp_status status =
        gui_session_client_prepare(
            client, session, &prepared, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_session *retired =
        gui_session_client_commit_prepared(
            client, &prepared);
    NT_ASSERT(retired == NULL);
    return TP_STATUS_OK;
}

tp_status gui_session_client_prepare(
    gui_session_client *client, tp_session *session,
    gui_session_client_prepared *prepared,
    tp_error *err) {
    if (!client || !session || !prepared) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session prepare requires client, session, and output");
    }
    if (prepared->session ||
        prepared->initial) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session prepare output is already occupied");
    }
    if (session == client->session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session prepare candidate aliases the active session");
    }
    if (client->frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session prepare is forbidden during a pinned frame");
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
    prepared->session = session;
    prepared->initial = initial;
    prepared->next_instance_generation =
        client->session_instance_generation + 1U;
    if (prepared->next_instance_generation ==
        0U) {
        prepared->next_instance_generation = 1U;
    }
    return TP_STATUS_OK;
}

void gui_session_client_cancel_prepared(
    gui_session_client_prepared *prepared) {
    if (!prepared) {
        return;
    }
    tp_session_observation_destroy(
        prepared->initial);
    memset(prepared, 0, sizeof *prepared);
}

tp_session *gui_session_client_commit_prepared(
    gui_session_client *client,
    gui_session_client_prepared *prepared) {
    NT_ASSERT(client != NULL);
    NT_ASSERT(prepared != NULL);
    NT_ASSERT(prepared->session != NULL);
    NT_ASSERT(prepared->initial != NULL);
    NT_ASSERT(!client->frame_pinned);
    tp_session *retired = client->session;
    const uint64_t next_generation =
        prepared->next_instance_generation;
    NT_ASSERT(next_generation != 0U);
    /* A successful attachment starts a distinct session-instance receipt
     * scope. Clear only after candidate observation preparation succeeded,
     * and before reducers see the new generation. */
    submit_state_clear(client);
    client->session = prepared->session;
    publish_observation(
        client, prepared->initial,
        next_generation);
    client->session_instance_generation =
        next_generation;
    client->admission_open = true;
    memset(prepared, 0, sizeof *prepared);
    return retired;
}

void gui_session_client_close_admission(
    gui_session_client *client) {
    NT_ASSERT(client != NULL);
    NT_ASSERT(client->session != NULL);
    client->admission_open = false;
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

tp_status gui_session_client_submit(
    gui_session_client *client,
    const gui_session_submit_request *request,
    gui_session_submit_result *out,
    tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
        out->terminal_status = TP_STATUS_OK;
        out->observation_status = TP_STATUS_OK;
    }
    if (!client || !client->session || !request || !out ||
        !request->operations || request->operation_count <= 0 ||
        request->operation_count > TP_TXN_MAX_OPS ||
        !request->semantic_label ||
        request->semantic_label[0] == '\0') {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI submit requires an attached client, operations, semantic label, and output");
    }
    /* A RETAINED id is resolved before the gates on purpose: it is the identified
     * (draft) shape, and the draft FSM stays in SUBMITTING until it reads back a
     * receipt carrying exactly this id -- so an admission/pin rejection must be
     * able to name it. A GENERATED id is resolved only after the gates: those are
     * the receipt-free structural intents (see gui_session_adapter.h), and a
     * rejected submit must not burn an id or a step of the transaction RNG. */
    char transaction_id[33] = {0};
    tp_status status = TP_STATUS_OK;
    if (request->retained_transaction_id) {
        status = submit_transaction_id(
            client, request->retained_transaction_id,
            transaction_id, err);
        if (status != TP_STATUS_OK) {
            /* A malformed retained id is the one case with no id to report; the
             * empty terminal transaction id is exactly the "the session never saw
             * this submit" answer the draft owner recovers from. */
            out->terminal_status = status;
            return status;
        }
    }
    if (!client->admission_open) {
        return reject_submit(
            client, request, transaction_id, out,
            "GUI session mutation admission is closed while lifecycle work drains",
            err);
    }
    if (client->frame_pinned) {
        return reject_submit(
            client, request, transaction_id, out,
            "GUI submit is forbidden during a pinned frame",
            err);
    }

    if (!request->retained_transaction_id) {
        bool unique = false;
        for (size_t attempt = 0U;
             attempt <
             GUI_SESSION_CLIENT_MAX_ID_GENERATION_ATTEMPTS;
             ++attempt) {
            status = submit_transaction_id(
                client, NULL, transaction_id, err);
            if (status != TP_STATUS_OK) {
                break;
            }
            if (!pending_find_id(
                    client, transaction_id)) {
                unique = true;
                break;
            }
        }
        if (status == TP_STATUS_OK && !unique) {
            status = tp_error_set(
                err, TP_STATUS_DUPLICATE_ID,
                "GUI transaction ID generation collided with retained receipts");
        }
        if (status != TP_STATUS_OK) {
            out->terminal_status = status;
            return status;
        }
    }

    gui_session_pending_submit *pending = NULL;
    status = pending_register_before_admission(
        client, transaction_id, request->identity,
        &pending, err);
    if (status != TP_STATUS_OK) {
        return fail_submit_with_receipt(
            client, request, transaction_id, out,
            status);
    }
    tp_txn_request transaction_request = {0};
    transaction_request.schema = TP_TXN_SCHEMA;
    (void)snprintf(
        transaction_request.id_hex,
        sizeof transaction_request.id_hex,
        "%s", transaction_id);
    transaction_request.expected_revision =
        request->expected_revision;
    transaction_request.label =
        (char *)request->semantic_label;
    transaction_request.author = "human";
    transaction_request.ops = request->operations;
    transaction_request.op_count =
        request->operation_count;

    status = tp_session_apply(
        client->session, &transaction_request,
        &out->transaction, err);
    out->terminal_status = status;
    if (status == TP_STATUS_DUPLICATE_ID) {
        /*
         * Core reports duplicate IDs with its current revision. Do not retain
         * that transient answer: a later retry must ask core again rather than
         * replaying a stale revision from this client. The CALLER still gets a
         * typed receipt for this attempt -- nothing is retained, but the draft
         * owner must never be left without an answer.
         */
        pending_clear(pending);
        return fail_submit_with_receipt(
            client, request, transaction_id, out, status);
    }
    pending_finish(
        client, pending, status, &out->transaction);
    out->terminal = pending->terminal;

    if (!out->transaction.committed) {
        return status;
    }

    if (!out->transaction.no_change) {
        tp_error observe_error = {{0}};
        out->observation_status =
            gui_session_client_observe(
                client, &observe_error);
        if (out->observation_status != TP_STATUS_OK) {
            out->observation_pending = true;
            pending->observation_status =
                out->observation_status;
        }
    }
    out->terminal = pending->terminal;
    /* Once core reports committed, a later recovery/observation failure is not
     * a mutation rejection. The typed receipt preserves both statuses. */
    return TP_STATUS_OK;
}

void gui_session_submit_result_destroy(
    gui_session_submit_result *result) {
    if (!result) {
        return;
    }
    tp_txn_result_free(&result->transaction);
    memset(result, 0, sizeof *result);
}

bool gui_session_client_pending_submit_query(
    const gui_session_client *client,
    const char transaction_id[33],
    gui_session_submit_identity identity,
    gui_session_submit_terminal *out) {
    if (!client || !transaction_id ||
        !transaction_id_is_canonical(transaction_id)) {
        return false;
    }
    for (size_t index = 0U;
         index < GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++index) {
        const gui_session_pending_submit *pending =
            &client->pending[index];
        if (pending_occupied(pending) &&
            strcmp(
                pending->terminal.transaction_id,
                transaction_id) == 0 &&
            submit_identity_equal(
                pending->terminal.identity,
                identity)) {
            if (out) {
                *out = pending->terminal;
            }
            return true;
        }
    }
    return false;
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
    client->admission_open = false;
    client->latest = NULL;
    client->snapshot_owner = NULL;
    client->observed =
        (tp_session_observation_token){0};
    submit_state_clear(client);
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

tp_session_job_observed_state
gui_session_client_job_state(
    const gui_session_client *client) {
    return client && client->latest
               ? tp_session_observation_job_state(
                     client->latest)
               : (tp_session_job_observed_state){0};
}

bool gui_session_client_frame_is_pinned(
    const gui_session_client *client) {
    return client && client->frame_pinned;
}

tp_session *gui_session_client_attached_session(
    const gui_session_client *client) {
    return client ? client->session : NULL;
}

#ifdef TP_ENABLE_TEST_SEAMS
void gui_session_client__test_fail_next_observe(void) {
    s_test_observe_failures = 1U;
}

void gui_session_client__test_fail_observes(
    unsigned int count) {
    s_test_observe_failures = count;
}

void gui_session_client__test_set_transaction_rng(
    gui_session_client *client, tp_rng rng) {
    if (client) {
        client->transaction_rng = rng;
    }
}
#endif
