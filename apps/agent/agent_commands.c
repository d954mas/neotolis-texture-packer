#include "agent_commands.h"

#include <stdint.h>
#include <string.h>
#include "core/nt_assert.h"
#include "tp_core/tp_sb.h"
#include "tp_core/tp_transaction.h"

#define JSON_SCHEMA_URI "https://json-schema.org/draft/2020-12/schema"
#define SAFE_REVISION_MAX INT64_C(9007199254740991)
#define REVISION_SCHEMA "{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991}"
#define HEX_SCHEMA "{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{32}$\"}"
#define COUNTER_SCHEMA "{\"type\":\"string\",\"pattern\":\"^(0|[1-9][0-9]*)$\",\"maxLength\":20}"
#define NULLABLE_TEXT "{\"type\":[\"string\",\"null\"]}"

typedef enum parameter_type {
    PARAM_STRING, PARAM_BOOL, PARAM_TRUE, PARAM_REVISION, PARAM_TRANSACTION,
    PARAM_ENUM
} parameter_type;

typedef struct parameter {
    const char *name;
    parameter_type type;
    bool required;
    const char *const *values;
    size_t value_count;
    const char *required_if_field;
    const char *required_if_value;
} parameter;

static const char *const close_decisions[] = {"preserve", "discard"};
static const parameter help_params[] = {{.name = "command", .type = PARAM_STRING}};
static const parameter bind_params[] = {{.name = "new", .type = PARAM_TRUE, .required = true}};
static const parameter close_params[] = {
    {.name = "decision", .type = PARAM_ENUM, .required = true,
     .values = close_decisions, .value_count = sizeof close_decisions / sizeof close_decisions[0]},
    {.name = "expected_revision", .type = PARAM_REVISION,
     .required_if_field = "decision", .required_if_value = "discard"}
};
static const parameter apply_params[] = {
    {.name = "transaction", .type = PARAM_TRANSACTION, .required = true},
    {.name = "dry_run", .type = PARAM_BOOL}
};
static const parameter history_params[] = {
    {.name = "expected_revision", .type = PARAM_REVISION, .required = true}
};

typedef struct command_row {
    agent_command command;
    const parameter *params;
    size_t param_count;
} command_row;

#define PARAMS(ARRAY) ARRAY, sizeof ARRAY / sizeof ARRAY[0]
static const command_row commands[] = {
    {{AGENT_HELP, "help", "Describe one supported command or all P1 commands.", false, false}, PARAMS(help_params)},
    {{AGENT_CAPABILITIES, "capabilities", "List implemented command and operation names, schema versions, and limits.", false, false}, NULL, 0},
    {{AGENT_OPERATIONS_LIST, "operations.list", "Describe current typed operations from the shared core catalog.", false, false}, NULL, 0},
    {{AGENT_SESSION_BIND, "session.bind", "Bind this process once to a new unsaved default project.", false, false}, PARAMS(bind_params)},
    {{AGENT_SESSION_STATUS, "session.status", "Observe session status, including while unbound or authorization is refused.", false, false}, NULL, 0},
    {{AGENT_SESSION_CLOSE, "session.close", "Close without Save; discard additionally requires host_generation and expected_revision.", false, false}, PARAMS(close_params)},
    {{AGENT_PROJECT_SNAPSHOT, "project.snapshot", "Read one coherent project/status snapshot without refreshing sources.", true, true}, NULL, 0},
    {{AGENT_PROJECT_APPLY, "project.apply", "Apply a core transaction, or preview it with dry_run=true; omitted dry_run is false.", true, true}, PARAMS(apply_params)},
    {{AGENT_HISTORY_LIST, "history.list", "Read the session history and status from the current host cut.", true, true}, NULL, 0},
    {{AGENT_HISTORY_UNDO, "history.undo", "Undo through the shared session at the expected revision.", true, true}, PARAMS(history_params)},
    {{AGENT_HISTORY_REDO, "history.redo", "Redo through the shared session at the expected revision.", true, true}, PARAMS(history_params)}
};
#undef PARAMS

size_t agent_command_count(void) { return sizeof commands / sizeof commands[0]; }

const agent_command *agent_command_at(size_t index) {
    return index < agent_command_count() ? &commands[index].command : NULL;
}

const agent_command *agent_command_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < agent_command_count(); ++i) {
        if (strcmp(commands[i].command.name, name) == 0) return &commands[i].command;
    }
    return NULL;
}

static bool value_valid(const parameter *field, const cJSON *value) {
    switch (field->type) {
        case PARAM_STRING: return cJSON_IsString(value);
        case PARAM_BOOL: return cJSON_IsBool(value);
        case PARAM_TRUE: return cJSON_IsTrue(value);
        case PARAM_TRANSACTION: return cJSON_IsObject(value);
        case PARAM_REVISION: {
            if (!cJSON_IsNumber(value)) return false;
            const double number = value->valuedouble;
            return number >= 0.0 && number <= (double)SAFE_REVISION_MAX &&
                   (double)(int64_t)number == number;
        }
        case PARAM_ENUM:
            if (!cJSON_IsString(value)) return false;
            for (size_t i = 0; i < field->value_count; ++i) {
                if (strcmp(field->values[i], value->valuestring) == 0) return true;
            }
            return false;
    }
    NT_ASSERT(false);
    return false;
}

bool agent_command_params_valid(const agent_command *command,
                                const cJSON *params, tp_error *err) {
    NT_ASSERT(command != NULL);
    NT_ASSERT((size_t)command->kind < agent_command_count());
    const command_row *row = &commands[command->kind];
    NT_ASSERT(command == &row->command);
    if (!cJSON_IsObject(params)) {
        (void)tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "params must be an object");
        return false;
    }
    for (const cJSON *item = params->child; item; item = item->next) {
        const parameter *field = NULL;
        for (size_t i = 0; i < row->param_count; ++i) {
            if (item->string && strcmp(item->string, row->params[i].name) == 0) {
                field = &row->params[i]; break;
            }
        }
        if (!field || cJSON_GetObjectItemCaseSensitive(params, field->name) != item) {
            (void)tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Unknown or duplicate parameter '%s'", item->string ? item->string : "");
            return false;
        }
        if (!value_valid(field, item)) {
            (void)tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Invalid parameter '%s'", field->name);
            return false;
        }
    }
    for (size_t i = 0; i < row->param_count; ++i) {
        const parameter *field = &row->params[i];
        bool required = field->required;
        if (field->required_if_field) {
            const cJSON *condition = cJSON_GetObjectItemCaseSensitive(params, field->required_if_field);
            required = required || (cJSON_IsString(condition) &&
                strcmp(condition->valuestring, field->required_if_value) == 0);
        }
        if (required && !cJSON_HasObjectItem(params, field->name)) {
            (void)tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Missing required parameter '%s'", field->name);
            return false;
        }
    }
    return true;
}

static void parameter_schema(tp_sb *out, const parameter *field) {
    switch (field->type) {
        case PARAM_STRING: tp_sb_str(out, "{\"type\":\"string\"}"); break;
        case PARAM_BOOL: tp_sb_str(out, "{\"type\":\"boolean\",\"default\":false}"); break;
        case PARAM_TRUE: tp_sb_str(out, "{\"const\":true}"); break;
        case PARAM_REVISION: tp_sb_str(out, REVISION_SCHEMA); break;
        case PARAM_TRANSACTION: {
            char *core_schema = tp_txn_request_schema_encode();
            if (!core_schema) { out->oom = true; break; }
            tp_sb_str(out, "{\"description\":\"TxnEnvelope: omit author; the host assigns it. Use operations.list for operation arguments.\",\"allOf\":[");
            tp_sb_str(out, core_schema);
            tp_sb_str(out, ",{\"properties\":{\"transaction\":{\"not\":{\"required\":[\"author\"]},\"properties\":{\"expected_revision\":");
            tp_sb_str(out, REVISION_SCHEMA);
            tp_sb_str(out, "}}}}]}");
            free(core_schema);
            break;
        }
        case PARAM_ENUM:
            tp_sb_str(out, "{\"type\":\"string\",\"enum\":[");
            for (size_t i = 0; i < field->value_count; ++i) {
                if (i) tp_sb_char(out, ',');
                tp_sb_json_string(out, field->values[i]);
            }
            tp_sb_str(out, "]}");
            break;
    }
}

static void params_schema(tp_sb *out, const command_row *row) {
    tp_sb_str(out, "{\"$schema\":\"" JSON_SCHEMA_URI "\",\"type\":\"object\",\"additionalProperties\":false,\"properties\":{");
    for (size_t i = 0; i < row->param_count; ++i) {
        if (i) tp_sb_char(out, ',');
        tp_sb_json_string(out, row->params[i].name);
        tp_sb_char(out, ':');
        parameter_schema(out, &row->params[i]);
    }
    tp_sb_str(out, "},\"required\":[");
    bool first = true;
    for (size_t i = 0; i < row->param_count; ++i) {
        if (!row->params[i].required) continue;
        if (!first) tp_sb_char(out, ',');
        first = false;
        tp_sb_json_string(out, row->params[i].name);
    }
    tp_sb_char(out, ']');
    first = true;
    for (size_t i = 0; i < row->param_count; ++i) {
        const parameter *field = &row->params[i];
        if (!field->required_if_field) continue;
        tp_sb_str(out, first ? ",\"allOf\":[" : ",");
        first = false;
        tp_sb_str(out, "{\"if\":{\"properties\":{");
        tp_sb_json_string(out, field->required_if_field);
        tp_sb_str(out, ":{\"const\":");
        tp_sb_json_string(out, field->required_if_value);
        tp_sb_str(out, "}},\"required\":[");
        tp_sb_json_string(out, field->required_if_field);
        tp_sb_str(out, "]},\"then\":{\"required\":[");
        tp_sb_json_string(out, field->name);
        tp_sb_str(out, "]}}");
    }
    if (!first) tp_sb_char(out, ']');
    tp_sb_char(out, '}');
}

static const char status_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"state\":{\"enum\":[\"bound\",\"authorization_pending\",\"host_lost\",\"closing\",\"closed\"]},"
    "\"observed\":{\"type\":\"boolean\"},\"session_id\":" HEX_SCHEMA ",\"host_generation\":" HEX_SCHEMA ","
    "\"host_kind\":{\"const\":\"headless\"},\"canonical_path\":{\"const\":null},\"revision\":" REVISION_SCHEMA ","
    "\"dirty\":{\"type\":\"boolean\"},\"event_sequence\":" COUNTER_SCHEMA ","
    "\"model_generation\":" COUNTER_SCHEMA ",\"source_generation\":" COUNTER_SCHEMA ","
    "\"authorization\":{\"enum\":[\"allowed\",\"pending\",\"denied\",\"disabled\"]},\"controller_id\":" HEX_SCHEMA ","
    "\"recovery\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},\"degraded\":{\"type\":\"boolean\"},\"code\":" NULLABLE_TEXT
    "},\"required\":[\"available\",\"degraded\",\"code\"]},\"job\":{\"const\":null}},"
    "\"required\":[\"state\",\"observed\",\"session_id\",\"host_generation\",\"host_kind\",\"canonical_path\","
    "\"revision\",\"dirty\",\"event_sequence\",\"model_generation\",\"source_generation\",\"authorization\","
    "\"controller_id\",\"recovery\",\"job\"]}";

static const char history_entry_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"kind\":{\"enum\":[\"edit\",\"save_checkpoint\",\"runtime_refresh\"]},\"revision\":" REVISION_SCHEMA ","
    "\"label\":" NULLABLE_TEXT ",\"author\":" NULLABLE_TEXT ",\"transaction_id\":{\"anyOf\":[" HEX_SCHEMA ",{\"type\":\"null\"}]},"
    "\"state_identity\":{\"anyOf\":[" HEX_SCHEMA ",{\"type\":\"null\"}]},\"path\":" NULLABLE_TEXT ","
    "\"undoable\":{\"type\":\"boolean\"},\"undone\":{\"type\":\"boolean\"}},"
    "\"required\":[\"kind\",\"revision\",\"label\",\"author\",\"transaction_id\",\"state_identity\",\"path\",\"undoable\",\"undone\"]}";

static void result_schema(tp_sb *out, agent_command_kind kind) {
    tp_sb_str(out, "{\"$schema\":\"" JSON_SCHEMA_URI "\",");
    bool has_status = false;
    switch (kind) {
        case AGENT_HELP:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"schema\":{\"const\":1},"
                "\"commands\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"command\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"host_generation\":{\"type\":\"boolean\"},"
                "\"params_schema\":{\"type\":\"object\"},\"result_schema\":{\"type\":\"object\"}},"
                "\"required\":[\"command\",\"description\",\"host_generation\",\"params_schema\",\"result_schema\"]}}},\"required\":[\"schema\",\"commands\"]");
            break;
        case AGENT_CAPABILITIES:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"agent_schema\":{\"const\":1},\"project_schema\":{\"const\":5},\"transaction_schema\":{\"const\":1},"
                "\"commands\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"operations\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
                "\"limits\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"request_bytes\":{\"const\":");
            tp_sb_uint(out, AGENT_MAX_REQUEST_BYTES);
            tp_sb_str(out, "},\"transaction_bytes\":{\"const\":"); tp_sb_uint(out, TP_TXN_MAX_REQUEST_BYTES);
            tp_sb_str(out, "},\"transaction_operations\":{\"const\":"); tp_sb_int(out, TP_TXN_MAX_OPS);
            tp_sb_str(out, "},\"retained_transaction_ids\":{\"const\":"); tp_sb_int(out, TP_TXN_RETAINED_ID_CAP);
            tp_sb_str(out, "}},\"required\":[\"request_bytes\",\"transaction_bytes\",\"transaction_operations\",\"retained_transaction_ids\"]}},"
                "\"required\":[\"agent_schema\",\"project_schema\",\"transaction_schema\",\"commands\",\"operations\",\"limits\"]");
            break;
        case AGENT_OPERATIONS_LIST:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"schema\":{\"const\":1},"
                "\"operations\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"op\":{\"type\":\"string\"},\"effect\":{\"enum\":[\"create\",\"remove\",\"move\",\"set\"]},"
                "\"target_kind\":{\"enum\":[\"atlas\",\"source\",\"animation\",\"target\"]},"
                "\"label\":{\"type\":\"string\"},\"label_template\":{\"type\":\"string\"},\"input_schema\":{\"type\":\"object\"}},"
                "\"required\":[\"op\",\"effect\",\"target_kind\",\"label\",\"label_template\",\"input_schema\"]}}},\"required\":[\"schema\",\"operations\"]");
            break;
        case AGENT_SESSION_BIND:
        case AGENT_PROJECT_SNAPSHOT:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"status\":{\"$ref\":\"#/$defs/status\"},\"project\":{\"$ref\":\"urn:ntpacker:project:5\"}},\"required\":[\"status\",\"project\"]");
            has_status = true;
            break;
        case AGENT_SESSION_STATUS:
            tp_sb_str(out, "\"anyOf\":[{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"state\":{\"const\":\"unbound\"}},\"required\":[\"state\"]},{\"$ref\":\"#/$defs/status\"}]");
            has_status = true;
            break;
        case AGENT_SESSION_CLOSE:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"closed\":{\"const\":true},\"preserved\":{\"type\":\"boolean\"}},\"required\":[\"closed\",\"preserved\"]");
            break;
        case AGENT_PROJECT_APPLY:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"mode\":{\"enum\":[\"apply\",\"dry_run\"]},\"transaction_result\":{\"$ref\":\"urn:ntpacker:transaction-result:1\"}},\"required\":[\"mode\",\"transaction_result\"]");
            break;
        case AGENT_HISTORY_LIST:
            tp_sb_str(out, "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                "\"status\":{\"$ref\":\"#/$defs/status\"},\"entries\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/history_entry\"}}},\"required\":[\"status\",\"entries\"]");
            has_status = true;
            break;
        case AGENT_HISTORY_UNDO:
        case AGENT_HISTORY_REDO:
            tp_sb_str(out, "\"$ref\":\"#/$defs/status\"");
            has_status = true;
            break;
        case AGENT_COMMAND_COUNT: NT_ASSERT(false); break;
    }
    if (has_status) {
        tp_sb_str(out, ",\"$defs\":{\"status\":"); tp_sb_str(out, status_schema);
        if (kind == AGENT_HISTORY_LIST) {
            tp_sb_str(out, ",\"history_entry\":"); tp_sb_str(out, history_entry_schema);
        }
        tp_sb_char(out, '}');
    }
    tp_sb_char(out, '}');
}

char *agent_help_encode(const char *optional_command) {
    const agent_command *selected = optional_command ? agent_command_find(optional_command) : NULL;
    if (optional_command && !selected) return NULL;
    tp_sb out = {0};
    tp_sb_str(&out, "{\"schema\":1,\"commands\":[");
    bool first = true;
    for (size_t i = 0; i < agent_command_count(); ++i) {
        const command_row *row = &commands[i];
        if (selected && selected != &row->command) continue;
        if (!first) tp_sb_char(&out, ',');
        first = false;
        tp_sb_str(&out, "{\"command\":"); tp_sb_json_string(&out, row->command.name);
        tp_sb_str(&out, ",\"description\":"); tp_sb_json_string(&out, row->command.description);
        tp_sb_str(&out, row->command.generation ? ",\"host_generation\":true,\"params_schema\":" : ",\"host_generation\":false,\"params_schema\":");
        params_schema(&out, row);
        tp_sb_str(&out, ",\"result_schema\":"); result_schema(&out, row->command.kind);
        tp_sb_char(&out, '}');
    }
    tp_sb_str(&out, "]}");
    if (out.oom) { tp_sb_free(&out); return NULL; }
    return out.buf;
}
