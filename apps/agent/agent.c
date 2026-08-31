#include "agent.h"
#include "agent_commands.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "core/nt_assert.h"
#include "app_automation_policy.h"
#include "app_json.h"
#include "app_paths.h"
#include "ntpacker_version.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_sb.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"

typedef struct agent_host {
    tp_format_catalog *catalog;
    tp_session *session;
    char controller[33];
    char generation[33];
    char session_id[33];
    char data_root[TP_IDENTITY_PATH_MAX];
    app_automation_mode mode;
    tp_status recovery_setup_status;
    bool closing;
    bool discarded;
    bool quiet;
    int exit_code;
} agent_host;

static const cJSON *member(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
static const char *string(const cJSON *value) {
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static bool text_equal(const cJSON *value, const char *text) {
    const char *value_text = string(value);
    return value_text && strcmp(value_text, text) == 0;
}

static void hex_id(tp_id128 id, char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < 16U; ++i) {
        out[i * 2U] = hex[id.bytes[i] >> 4U];
        out[i * 2U + 1U] = hex[id.bytes[i] & 15U];
    }
    out[32] = '\0';
}

static tp_status new_id(char out[33], tp_error *err) {
    tp_rng rng = tp_rng_os();
    tp_id128 id;
    const tp_status status = tp_id128_generate(&rng, &id, err);
    if (status != TP_STATUS_OK) return status;
    hex_id(id, out);
    return TP_STATUS_OK;
}

/* Core encoders own their payload shape. Remove only insignificant whitespace
 * to carry their exact numeric/string spellings on one physical output line. */
static void compact_json(tp_sb *out, const char *json, size_t length) {
    bool quoted = false;
    for (size_t i = 0U; i < length; ++i) {
        const char ch = json[i];
        if (quoted) {
            tp_sb_char(out, ch);
            if (ch == '\\' && i + 1U < length) tp_sb_char(out, json[++i]);
            else if (ch == '"') quoted = false;
        } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            tp_sb_char(out, ch);
            if (ch == '"') quoted = true;
        }
    }
}

static tp_status recovery_problem(const agent_host *host) {
    if (!host->session) return TP_STATUS_OK;
    const tp_session_recovery_health health = tp_session_recovery_health_query(host->session);
    if (health.degraded) return health.first_cause;
    if (!health.available) return host->recovery_setup_status != TP_STATUS_OK
        ? host->recovery_setup_status : TP_STATUS_JOURNAL_FAILED;
    return TP_STATUS_OK;
}

static void notices(tp_sb *out, const agent_host *host) {
    tp_sb_str(out, ",\"notices\":[");
    if (host->session) {
        const tp_status problem = recovery_problem(host);
        if (problem != TP_STATUS_OK) {
            tp_sb_str(out, "{\"code\":\"recovery_degraded\",\"message\":\"Unsaved changes may not survive exit\",\"details\":{\"code\":");
            tp_sb_json_string(out, tp_status_id(problem));
            tp_sb_str(out, "}}");
        }
    }
    tp_sb_str(out, "]}");
}

static void begin_response(tp_sb *out, const char *id, bool ok) {
    tp_sb_str(out, "{\"schema\":1,\"id\":");
    if (id) tp_sb_json_string(out, id); else tp_sb_str(out, "null");
    tp_sb_str(out, ok ? ",\"ok\":true,\"result\":" : ",\"ok\":false,\"error\":");
}
static void error_response(tp_sb *out, const agent_host *host, const char *id,
                           const char *code, const char *message) {
    begin_response(out, id, false);
    tp_sb_str(out, "{\"code\":"); tp_sb_json_string(out, code);
    tp_sb_str(out, ",\"message\":"); tp_sb_json_string(out, message);
    tp_sb_str(out, ",\"details\":{}}"); notices(out, host);
}

static bool write_response(const tp_sb *response) {
    return !response->oom && response->buf &&
        fwrite(response->buf, 1U, response->len, stdout) == response->len &&
        fputc('\n', stdout) != EOF && fflush(stdout) == 0;
}

static void status_json(tp_sb *out, const agent_host *host) {
    if (!host->session) { tp_sb_str(out, "{\"state\":\"unbound\"}"); return; }
    const struct tp_session_view *view = tp_session_view(host->session);
    NT_ASSERT(view != NULL);
    const tp_session_snapshot *snapshot = view->snapshot;
    const tp_session_recovery_health health = view->recovery_health;
    tp_sb_str(out, host->mode == APP_AUTOMATION_DISABLED
        ? "{\"state\":\"authorization_pending\",\"observed\":true,\"session_id\":"
        : "{\"state\":\"bound\",\"observed\":true,\"session_id\":");
    tp_sb_json_string(out, host->session_id);
    tp_sb_str(out, ",\"host_generation\":"); tp_sb_json_string(out, host->generation);
    tp_sb_str(out, ",\"host_kind\":\"headless\",\"canonical_path\":null,\"revision\":");
    tp_sb_i64(out, tp_session_snapshot_revision(snapshot));
    tp_sb_str(out, ",\"dirty\":");
    tp_sb_str(out, tp_session_snapshot_dirty(snapshot) ? "true" : "false");
    char counters[192];
    const int count = snprintf(counters, sizeof counters, ",\"event_sequence\":\"%" PRIu64 "\",\"model_generation\":\"%" PRIu64 "\",\"source_generation\":\"%" PRIu64 "\"",
        tp_session_snapshot_event_sequence(snapshot),
        tp_session_snapshot_model_generation(snapshot),
        tp_session_snapshot_source_generation(snapshot));
    NT_ASSERT(count > 0 && (size_t)count < sizeof counters);
    tp_sb_str(out, counters);
    tp_sb_str(out, host->mode == APP_AUTOMATION_DISABLED
        ? ",\"authorization\":\"disabled\",\"controller_id\":"
        : ",\"authorization\":\"allowed\",\"controller_id\":");
    tp_sb_json_string(out, host->controller);
    tp_sb_str(out, ",\"recovery\":{\"available\":");
    tp_sb_str(out, health.available ? "true" : "false");
    const tp_status recovery_status = recovery_problem(host);
    tp_sb_str(out, ",\"degraded\":"); tp_sb_str(out, recovery_status != TP_STATUS_OK ? "true" : "false");
    tp_sb_str(out, ",\"code\":");
    if (recovery_status != TP_STATUS_OK) tp_sb_json_string(out, tp_status_id(recovery_status));
    else tp_sb_str(out, "null");
    tp_sb_str(out, "},\"job\":null}");
}

static tp_status snapshot_json(tp_sb *out, const agent_host *host, tp_error *err) {
    char *project = NULL;
    size_t length = 0U;
    const struct tp_session_view *view = tp_session_view(host->session);
    NT_ASSERT(view != NULL);
    const tp_status status = tp_session_snapshot_serialize(view->snapshot, &project, &length, err);
    if (status != TP_STATUS_OK) return status;
    tp_sb_str(out, "{\"status\":"); status_json(out, host);
    tp_sb_str(out, ",\"project\":"); compact_json(out, project, length);
    tp_sb_char(out, '}');
    free(project);
    return TP_STATUS_OK;
}

static tp_status bind_new(agent_host *host, tp_error *err) {
    tp_status status = app_automation_policy_read(host->data_root, &host->mode, err);
    if (status != TP_STATUS_OK || host->mode == APP_AUTOMATION_DISABLED) return status;
    tp_rng rng = tp_rng_os();
    status = new_id(host->generation, err);
    if (status == TP_STATUS_OK) status = tp_session_create_default_project_with_catalog(host->catalog, &rng, &host->session, err);
    if (status != TP_STATUS_OK) return status;
    status = tp_session_require_recovery(host->session, err);
    NT_ASSERT(status == TP_STATUS_OK);
    char recovery[TP_IDENTITY_PATH_MAX];
    const int n = snprintf(recovery, sizeof recovery, "%s/recovery", host->data_root);
    if (n >= 0 && (size_t)n < sizeof recovery) {
        tp_mkdirs(recovery);
        const tp_recovery_metadata metadata = {.timestamp = (int64_t)time(NULL), .project_path = "", .project_name = "untitled"};
        host->recovery_setup_status = tp_recovery_session_attach(recovery, app_recovery_key(), &rng, host->session, &metadata, err);
    } else host->recovery_setup_status = TP_STATUS_OUT_OF_BOUNDS;
    status = tp_session_update(host->session, NULL, err);
    if (status != TP_STATUS_OK) {
        tp_session_destroy(host->session);
        host->session = NULL;
        return status;
    }
    const struct tp_session_view *view = tp_session_view(host->session);
    hex_id(tp_session_snapshot_identity(view->snapshot).session_id, host->session_id);
    return TP_STATUS_OK;
}

static void history_json(tp_sb *out, const agent_host *host) {
    tp_sb_str(out, "{\"status\":"); status_json(out, host);
    tp_sb_str(out, ",\"entries\":[");
    const int count = tp_session_history_count(host->session);
    for (int i = 0; i < count; ++i) {
        tp_session_history_entry entry;
        const tp_status status = tp_session_history_at(host->session, i, &entry, NULL);
        NT_ASSERT(status == TP_STATUS_OK);
        if (i != 0) tp_sb_char(out, ',');
        tp_sb_str(out, "{\"kind\":");
        const char *kind = entry.kind == TP_SESSION_HISTORY_EDIT ? "edit" :
            entry.kind == TP_SESSION_HISTORY_SAVE_CHECKPOINT ? "save_checkpoint" : "runtime_refresh";
        tp_sb_json_string(out, kind);
        tp_sb_str(out, ",\"revision\":"); tp_sb_i64(out, entry.revision);
        const char *const names[] = {"label", "author", "transaction_id", "path"};
        const char *const values[] = {entry.label, entry.author, entry.transaction_id, entry.path};
        for (size_t j = 0U; j < 4U; ++j) {
            tp_sb_char(out, ','); tp_sb_json_string(out, names[j]); tp_sb_char(out, ':');
            if (values[j][0]) tp_sb_json_string(out, values[j]); else tp_sb_str(out, "null");
        }
        tp_sb_str(out, ",\"state_identity\":");
        if (tp_id128_is_nil(entry.state_identity)) tp_sb_str(out, "null");
        else { char id[33]; hex_id(entry.state_identity, id); tp_sb_json_string(out, id); }
        tp_sb_str(out, ",\"undoable\":"); tp_sb_str(out, entry.undoable ? "true" : "false");
        tp_sb_str(out, ",\"undone\":"); tp_sb_str(out, entry.undone ? "true" : "false");
        tp_sb_char(out, '}');
    }
    tp_sb_str(out, "]}");
}

static void capabilities_json(tp_sb *out) {
    tp_sb_str(out, "{\"agent_schema\":1,\"project_schema\":5,\"transaction_schema\":1,\"commands\":[");
    for (size_t i = 0U; i < agent_command_count(); ++i) {
        if (i != 0U) tp_sb_char(out, ',');
        tp_sb_json_string(out, agent_command_at(i)->name);
    }
    tp_sb_str(out, "],\"operations\":[");
    bool first = true;
    for (int k = TP_OP_INVALID + 1; k < TP_OP_KIND_COUNT; ++k) {
        const tp_op_info *info = tp_op_info_by_kind((tp_op_kind)k);
        if (!info->cli_verb) continue;
        if (!first) tp_sb_char(out, ',');
        first = false; tp_sb_json_string(out, info->wire);
    }
    tp_sb_str(out, "],\"limits\":{\"request_bytes\":"); tp_sb_size(out, AGENT_MAX_REQUEST_BYTES);
    tp_sb_str(out, ",\"transaction_bytes\":"); tp_sb_int(out, TP_TXN_MAX_REQUEST_BYTES);
    tp_sb_str(out, ",\"transaction_operations\":"); tp_sb_int(out, TP_TXN_MAX_OPS);
    tp_sb_str(out, ",\"retained_transaction_ids\":"); tp_sb_int(out, TP_TXN_RETAINED_ID_CAP);
    tp_sb_str(out, "}}");
}

static bool integer_member(const char *bytes, size_t length,
                           const cJSON *object, const char *key) {
    size_t number_length = 0U;
    const char *number = app_json_member_span(bytes, length, object, key, &number_length);
    return cJSON_IsNumber(member(object, key)) && app_json_safe_integer(number, number_length);
}

static void transaction_reply(agent_host *host, const char *line, size_t length,
                               const cJSON *root, const char *id, tp_sb *out) {
    const cJSON *params = member(root, "params");
    const bool preview = cJSON_IsTrue(member(params, "dry_run"));
    const char *mode = preview ? "dry_run" : "apply";
    size_t params_length = 0U, transaction_length = 0U;
    const char *params_bytes = app_json_member_span(line, length, root, "params", &params_length);
    const char *transaction_bytes = app_json_member_span(params_bytes, params_length, params, "transaction", &transaction_length);
    tp_error err = {0};
    tp_txn_request *request = NULL;
    tp_status status = TP_STATUS_OK;
    const cJSON *envelope = member(params, "transaction");
    const cJSON *transaction = member(envelope, "transaction");
    if (member(envelope, "schema") && !integer_member(transaction_bytes, transaction_length, envelope, "schema")) {
        status = tp_error_set(&err, TP_STATUS_INVALID_ARGUMENT, "Transaction schema must be an exact integer");
    }
    if (status == TP_STATUS_OK && cJSON_IsObject(transaction) && member(transaction, "expected_revision")) {
        size_t inner_length = 0U;
        const char *inner_bytes = app_json_member_span(transaction_bytes, transaction_length, envelope, "transaction", &inner_length);
        if (!integer_member(inner_bytes, inner_length, transaction, "expected_revision")) {
            status = tp_error_set(&err, TP_STATUS_INVALID_ARGUMENT, "expected_revision must be an exact JSON-safe integer");
        }
    }
    if (status == TP_STATUS_OK) status = tp_txn_request_decode_n(transaction_bytes, transaction_length, &request, &err);
    if (status == TP_STATUS_OK && member(member(member(params, "transaction"), "transaction"), "author")) {
        status = tp_error_set(&err, TP_STATUS_INVALID_ARGUMENT, "Agent transaction author is assigned by the host");
    }
    for (int i = 0; status == TP_STATUS_OK && i < request->op_count; ++i) {
        const tp_op_info *info = tp_op_info_by_kind(request->ops[i].kind);
        NT_ASSERT(info != NULL);
        /* The same catalog predicate drives capabilities and operations.list.
         * Reserved core wire slots are not current agent capabilities. */
        if (!info->cli_verb) {
            status = tp_error_set(&err, TP_STATUS_UNKNOWN_OP,
                "Operation '%s' is not available in agent mode", info->wire);
        }
    }
    if (status != TP_STATUS_OK) {
        begin_response(out, id, false);
        tp_sb_str(out, "{\"code\":"); tp_sb_json_string(out, tp_status_id(status));
        tp_sb_str(out, ",\"message\":"); tp_sb_json_string(out, err.msg);
        tp_sb_str(out, ",\"details\":{\"phase\":\"decode\",\"field\":null}}");
        notices(out, host); tp_txn_request_free(request); return;
    }
    char author[40];
    (void)snprintf(author, sizeof author, "agent(%s)", host->controller);
    request->author = (char *)malloc(strlen(author) + 1U);
    if (!request->author) {
        error_response(out, host, id, "oom", "Could not prepare transaction authorship");
        tp_txn_request_free(request); return;
    }
    memcpy(request->author, author, strlen(author) + 1U);
    tp_txn_result result = {0};
    if (preview) status = tp_session_snapshot_apply_preview(tp_session_view(host->session)->snapshot, request, &result, &err);
    else status = tp_session_apply(host->session, request, &result, &err);
    char *encoded = tp_txn_result_encode(&result);
    if (!encoded) {
        /* A wet commit may already have happened. Closing without a complete
         * reply communicates uncertainty; never fabricate a rejected commit. */
        out->oom = true;
    } else {
        const bool accepted = result.committed || result.no_change;
        begin_response(out, id, accepted);
        if (!accepted) {
            tp_sb_str(out, "{\"code\":"); tp_sb_json_string(out, tp_status_id(status));
            tp_sb_str(out, ",\"message\":"); tp_sb_json_string(out, err.msg);
            tp_sb_str(out, ",\"details\":{\"phase\":\"admission\",");
        } else tp_sb_char(out, '{');
        tp_sb_str(out, "\"mode\":"); tp_sb_json_string(out, mode);
        tp_sb_str(out, ",\"transaction_result\":"); compact_json(out, encoded, strlen(encoded));
        tp_sb_char(out, '}');
        if (!accepted) tp_sb_char(out, '}');
        notices(out, host);
    }
    free(encoded); tp_txn_result_free(&result); tp_txn_request_free(request);
}

static bool request_id(const char *id) {
    if (!id || !id[0] || strlen(id) > 64U) return false;
    for (const char *p = id; *p; ++p) {
        if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') &&
            (*p < '0' || *p > '9') && *p != '_' && *p != '.' && *p != '-') return false;
    }
    return true;
}

static bool generation_token(const cJSON *value) {
    const char *text = string(value);
    if (!text || strlen(text) != 32U) return false;
    for (size_t i = 0U; i < 32U; ++i) {
        if ((text[i] < '0' || text[i] > '9') && (text[i] < 'a' || text[i] > 'f')) return false;
    }
    return true;
}

static void dispatch(agent_host *host, const char *line, size_t length, tp_sb *out) {
    tp_error err = {0};
    cJSON *root = app_json_parse(line, length, &err);
    const char *id = root ? string(member(root, "id")) : NULL;
    if (!request_id(id)) id = NULL;
    const char *const keys[] = {"schema", "id", "command", "params", "host_generation"};
    if (!root || !app_json_object_fields(root, keys, 5U) || !id ||
        !cJSON_IsNumber(member(root, "schema")) || !string(member(root, "command")) ||
        !cJSON_IsObject(member(root, "params")) ||
        (member(root, "host_generation") && !generation_token(member(root, "host_generation")))) {
        error_response(out, host, id, "bad_request", "Invalid request envelope");
        cJSON_Delete(root); return;
    }
    if (member(root, "schema")->valuedouble != 1.0 || !integer_member(line, length, root, "schema")) {
        error_response(out, host, id, "unsupported_schema", "Expected agent schema 1");
        cJSON_Delete(root); return;
    }
    const agent_command *command = agent_command_find(string(member(root, "command")));
    const cJSON *params = member(root, "params");
    if (!command) {
        error_response(out, host, id, "unknown_command", "Command is not available in this packet");
        cJSON_Delete(root); return;
    }
    if (!agent_command_params_valid(command, params, &err)) {
        error_response(out, host, id, "invalid_argument", err.msg);
        cJSON_Delete(root); return;
    }
    size_t params_length = 0U;
    const char *params_bytes = app_json_member_span(line, length, root, "params", &params_length);
    if (member(params, "expected_revision") &&
        !integer_member(params_bytes, params_length, params, "expected_revision")) {
        error_response(out, host, id, "invalid_argument", "expected_revision must be an exact JSON-safe integer");
        cJSON_Delete(root); return;
    }
    const bool discard = command->kind == AGENT_SESSION_CLOSE && text_equal(member(params, "decision"), "discard");
    if ((command->bound || discard) && !host->session) {
        error_response(out, host, id, "not_bound", "Bind a project first");
        cJSON_Delete(root); return;
    }
    if ((command->generation || discard) && !text_equal(member(root, "host_generation"), host->generation)) {
        error_response(out, host, id, "host_changed", "Observe the current host generation");
        cJSON_Delete(root); return;
    }
    if (host->session) {
        const tp_status policy = app_automation_policy_read(host->data_root, &host->mode, &err);
        if ((command->bound || discard) && (policy != TP_STATUS_OK || host->mode == APP_AUTOMATION_DISABLED)) {
            error_response(out, host, id, policy != TP_STATUS_OK ? tp_status_id(policy) : "authorization_disabled",
                err.msg[0] ? err.msg : "Agent access is disabled");
            cJSON_Delete(root); return;
        }
        const tp_status updated = tp_session_update(host->session, NULL, &err);
        if (updated != TP_STATUS_OK) {
            error_response(out, host, id, tp_status_id(updated), err.msg);
            cJSON_Delete(root); return;
        }
    }
    switch (command->kind) {
        case AGENT_HELP:
        case AGENT_OPERATIONS_LIST: {
            const char *filter = string(member(params, "command"));
            if (filter && !agent_command_find(filter)) {
                error_response(out, host, id, "unknown_command", "No help for this command"); break;
            }
            char *encoded = command->kind == AGENT_HELP ? agent_help_encode(filter) : tp_op_catalog_encode();
            if (!encoded) { error_response(out, host, id, "oom", "Could not encode command metadata"); break; }
            begin_response(out, id, true); compact_json(out, encoded, strlen(encoded)); notices(out, host);
            free(encoded); break;
        }
        case AGENT_CAPABILITIES:
            begin_response(out, id, true); capabilities_json(out); notices(out, host); break;
        case AGENT_SESSION_BIND: {
            if (host->session) { error_response(out, host, id, "already_bound", "This process is already bound"); break; }
            const tp_status status = bind_new(host, &err);
            if (status != TP_STATUS_OK || !host->session) {
                error_response(out, host, id, status != TP_STATUS_OK ? tp_status_id(status) : "authorization_disabled",
                    err.msg[0] ? err.msg : "Agent access is disabled"); break;
            }
            begin_response(out, id, true);
            if (snapshot_json(out, host, &err) != TP_STATUS_OK) out->oom = true;
            notices(out, host); break;
        }
        case AGENT_SESSION_STATUS:
            begin_response(out, id, true); status_json(out, host); notices(out, host); break;
        case AGENT_SESSION_CLOSE: {
            if (discard) {
                const int64_t expected = (int64_t)member(params, "expected_revision")->valuedouble;
                tp_status status = tp_revision_check(expected, tp_session_revision(host->session), &err);
                if (status == TP_STATUS_OK) status = tp_session_discard(host->session, &err);
                if (status != TP_STATUS_OK) { error_response(out, host, id, tp_status_id(status), err.msg); break; }
                host->discarded = true;
            }
            host->closing = true;
            host->exit_code = recovery_problem(host) != TP_STATUS_OK && !discard ? 1 : 0;
            begin_response(out, id, true);
            tp_sb_str(out, discard || host->exit_code ? "{\"closed\":true,\"preserved\":false}" : "{\"closed\":true,\"preserved\":true}");
            notices(out, host); break;
        }
        case AGENT_PROJECT_SNAPSHOT: {
            /* Snapshot allocation precedes any effect; encoding failure here
             * can be an ordinary rejection. */
            tp_sb snapshot = {0};
            const tp_status status = snapshot_json(&snapshot, host, &err);
            if (status != TP_STATUS_OK || snapshot.oom) error_response(out, host, id, "oom", "Could not prepare snapshot");
            else { begin_response(out, id, true); tp_sb_str(out, snapshot.buf); notices(out, host); }
            tp_sb_free(&snapshot); break;
        }
        case AGENT_PROJECT_APPLY:
            transaction_reply(host, line, length, root, id, out); break;
        case AGENT_HISTORY_LIST:
            begin_response(out, id, true); history_json(out, host); notices(out, host); break;
        case AGENT_HISTORY_UNDO:
        case AGENT_HISTORY_REDO: {
            const int64_t expected = (int64_t)member(params, "expected_revision")->valuedouble;
            tp_status status = tp_revision_check(expected, tp_session_revision(host->session), &err);
            if (status == TP_STATUS_OK) status = command->kind == AGENT_HISTORY_UNDO
                ? tp_session_undo(host->session, &err) : tp_session_redo(host->session, &err);
            if (status != TP_STATUS_OK) error_response(out, host, id, tp_status_id(status), err.msg);
            else if (tp_session_update(host->session, NULL, &err) != TP_STATUS_OK) out->oom = true;
            else { begin_response(out, id, true); status_json(out, host); notices(out, host); }
            break;
        }
        case AGENT_COMMAND_COUNT: NT_ASSERT(false); break;
    }
    cJSON_Delete(root);
}

static bool blank_line(const char *line, size_t length) {
    for (size_t i = 0U; i < length; ++i) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return false;
    }
    return true;
}

int ntpacker_agent_main(int argc, char **argv, tp_format_catalog *catalog) {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#else
    (void)signal(SIGPIPE, SIG_IGN);
#endif
    agent_host host = {.catalog = catalog, .mode = APP_AUTOMATION_ASK};
    bool create = false;
    bool help = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--new") == 0 && !create) create = true;
        else if (strcmp(argv[i], "--quiet") == 0) host.quiet = true;
        else if (strcmp(argv[i], "--help") == 0) help = true;
        else if (strcmp(argv[i], "--json") != 0) {
            tp_sb error = {0};
            error_response(&error, &host, NULL, "bad_request", "Unsupported agent startup option");
            (void)write_response(&error); tp_sb_free(&error); return 2;
        }
    }
    if (help) {
        char *encoded = agent_help_encode(NULL);
        tp_sb result = {0};
        if (encoded) compact_json(&result, encoded, strlen(encoded));
        else error_response(&result, &host, NULL, "oom", "Could not encode help");
        const bool written = write_response(&result);
        const int code = written && encoded ? 0 : 1;
        tp_sb_free(&result); free(encoded); return code;
    }
    tp_error err = {0};
    tp_status status = new_id(host.controller, &err);
    const bool have_root = app_paths_data_root(host.data_root, sizeof host.data_root, false);
    if (status == TP_STATUS_OK && create && have_root) status = bind_new(&host, &err);
    tp_sb ready = {0};
    if (status != TP_STATUS_OK || (create && (!have_root || !host.session))) {
        const char *code = status != TP_STATUS_OK ? tp_status_id(status) : "authorization_disabled";
        error_response(&ready, &host, NULL, code, err.msg[0] ? err.msg : "Agent authorization unavailable");
        (void)write_response(&ready); tp_sb_free(&ready);
        tp_session_destroy(host.session);
        return status == TP_STATUS_OOM || status == TP_STATUS_RNG_FAILED ? 1 : 8;
    }
    tp_sb_str(&ready, "{\"schema\":1,\"type\":\"ready\",\"app_version\":");
    tp_sb_json_string(&ready, NTPACKER_VERSION);
    tp_sb_str(&ready, ",\"controller_id\":"); tp_sb_json_string(&ready, host.controller);
    tp_sb_str(&ready, create ? ",\"state\":\"bound\",\"session\":" : ",\"state\":\"unbound\"");
    if (create && snapshot_json(&ready, &host, &err) != TP_STATUS_OK) ready.oom = true;
    tp_sb_char(&ready, '}');
    bool output_ok = write_response(&ready); tp_sb_free(&ready);
    char *line = (char *)malloc(AGENT_MAX_REQUEST_BYTES + 2U);
    if (!line) output_ok = false;
    size_t length = 0U;
    while (output_ok && !host.closing) {
        const int ch = fgetc(stdin);
        if (ch == EOF) { if (ferror(stdin) || !blank_line(line, length)) host.exit_code = 1; break; }
        if (ch == '\n') {
            if (length && line[length - 1U] == '\r') --length;
            if (!blank_line(line, length)) {
                line[length] = '\0';
                tp_sb response = {0}; dispatch(&host, line, length, &response);
                output_ok = write_response(&response); tp_sb_free(&response);
            }
            length = 0U;
        } else if (length >= AGENT_MAX_REQUEST_BYTES &&
                   (length > AGENT_MAX_REQUEST_BYTES || ch != '\r')) {
            tp_sb error = {0}; error_response(&error, &host, NULL, "request_too_large", "Agent line exceeds 2 MiB");
            (void)write_response(&error); tp_sb_free(&error);
            host.exit_code = 1; break;
        } else line[length++] = (char)ch;
    }
    free(line);
    if (!output_ok) {
        host.exit_code = 1;
        if (!host.quiet) (void)fputs("ntpacker agent: output delivery failed; command outcome is unknown\n", stderr);
    }
    if (!host.discarded && recovery_problem(&host) != TP_STATUS_OK) host.exit_code = 1;
    tp_session_destroy(host.session);
    return host.exit_code;
}
