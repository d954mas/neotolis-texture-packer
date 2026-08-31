#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "agent_commands.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const cJSON *member(const cJSON *object, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(value, key);
    return value;
}

static bool string_in_array(const cJSON *array, const char *text) {
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, text) == 0) return true;
    }
    return false;
}

static bool params_valid(const char *command, const char *json) {
    cJSON *params = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(params);
    tp_error error = {0};
    const bool valid = agent_command_params_valid(agent_command_find(command), params, &error);
    if (!valid) TEST_ASSERT_NOT_EMPTY(error.msg);
    cJSON_Delete(params);
    return valid;
}

static void current_command_catalog_is_closed(void) {
    static const char *const expected[] = {
        "help", "capabilities", "operations.list", "session.bind", "session.status",
        "session.close", "project.snapshot", "project.apply", "history.list",
        "history.undo", "history.redo"
    };
    TEST_ASSERT_EQUAL_UINT(sizeof expected / sizeof expected[0], agent_command_count());
    for (size_t i = 0; i < agent_command_count(); ++i) {
        const agent_command *command = agent_command_at(i);
        TEST_ASSERT_NOT_NULL(command);
        TEST_ASSERT_EQUAL_STRING(expected[i], command->name);
        TEST_ASSERT_EQUAL_PTR(command, agent_command_find(expected[i]));
        TEST_ASSERT_EQUAL_INT((int)i, command->kind);
        TEST_ASSERT_NOT_EMPTY(command->description);
    }
    TEST_ASSERT_NULL(agent_command_at(agent_command_count()));
    TEST_ASSERT_NULL(agent_command_find("pack.start"));
    TEST_ASSERT_NULL(agent_command_find("session.bind.path"));
    TEST_ASSERT_NULL(agent_command_find(NULL));
    TEST_ASSERT_FALSE(agent_command_find("session.status")->bound);
    TEST_ASSERT_FALSE(agent_command_find("session.close")->generation);
    TEST_ASSERT_TRUE(agent_command_find("history.undo")->bound);
    TEST_ASSERT_TRUE(agent_command_find("history.undo")->generation);
}

static void flat_params_admit_only_documented_values(void) {
    TEST_ASSERT_TRUE(params_valid("help", "{}"));
    TEST_ASSERT_TRUE(params_valid("help", "{\"command\":\"session.status\"}"));
    TEST_ASSERT_FALSE(params_valid("help", "{\"command\":false}"));
    TEST_ASSERT_TRUE(params_valid("session.bind", "{\"new\":true}"));
    TEST_ASSERT_FALSE(params_valid("session.bind", "{}"));
    TEST_ASSERT_FALSE(params_valid("session.bind", "{\"new\":false}"));
    TEST_ASSERT_FALSE(params_valid("session.bind", "{\"new\":1}"));
    TEST_ASSERT_FALSE(params_valid("session.bind", "{\"path\":\"/tmp/project\"}"));
    TEST_ASSERT_FALSE(params_valid("session.status", "[]"));
    TEST_ASSERT_FALSE(params_valid("session.status", "{\"extra\":1}"));
    TEST_ASSERT_TRUE(params_valid("project.apply", "{\"transaction\":{}}"));
    TEST_ASSERT_TRUE(params_valid("project.apply", "{\"transaction\":{},\"dry_run\":true}"));
    TEST_ASSERT_FALSE(params_valid("project.apply", "{\"transaction\":null}"));
    TEST_ASSERT_FALSE(params_valid("project.apply", "{\"transaction\":{},\"dry_run\":1}"));
    TEST_ASSERT_FALSE(params_valid("project.apply", "{\"transaction\":{},\"transaction\":{}}"));
}

static void discard_and_history_require_safe_revision(void) {
    TEST_ASSERT_TRUE(params_valid("session.close", "{\"decision\":\"preserve\"}"));
    TEST_ASSERT_TRUE(params_valid("session.close", "{\"decision\":\"discard\",\"expected_revision\":0}"));
    TEST_ASSERT_FALSE(params_valid("session.close", "{\"decision\":\"discard\"}"));
    TEST_ASSERT_FALSE(params_valid("session.close", "{\"decision\":\"save\"}"));
    TEST_ASSERT_FALSE(params_valid("session.close", "{\"decision\":\"preserve\",\"expected_revision\":-1}"));
    TEST_ASSERT_TRUE(params_valid("history.undo", "{\"expected_revision\":9007199254740991}"));
    TEST_ASSERT_TRUE(params_valid("history.redo", "{\"expected_revision\":0}"));
    TEST_ASSERT_FALSE(params_valid("history.undo", "{\"expected_revision\":9007199254740992}"));
    TEST_ASSERT_FALSE(params_valid("history.undo", "{\"expected_revision\":0.5}"));
    TEST_ASSERT_FALSE(params_valid("history.undo", "{\"expected_revision\":\"0\"}"));
    TEST_ASSERT_FALSE(params_valid("history.redo", "{}"));
}

static void help_exposes_command_and_result_shapes(void) {
    char *text = agent_help_encode(NULL);
    TEST_ASSERT_NOT_NULL(text);
    cJSON *help = cJSON_Parse(text);
    free(text);
    TEST_ASSERT_NOT_NULL(help);
    TEST_ASSERT_EQUAL_INT(1, member(help, "schema")->valueint);
    const cJSON *commands = member(help, "commands");
    TEST_ASSERT_EQUAL_INT(AGENT_COMMAND_COUNT, cJSON_GetArraySize(commands));
    for (int i = 0; i < AGENT_COMMAND_COUNT; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(commands, i);
        const agent_command *command = agent_command_at((size_t)i);
        TEST_ASSERT_EQUAL_STRING(command->name, member(entry, "command")->valuestring);
        TEST_ASSERT_EQUAL_STRING(command->description, member(entry, "description")->valuestring);
        TEST_ASSERT_EQUAL_INT(command->generation, cJSON_IsTrue(member(entry, "host_generation")));
        const cJSON *params = member(entry, "params_schema");
        TEST_ASSERT_EQUAL_STRING("https://json-schema.org/draft/2020-12/schema", member(params, "$schema")->valuestring);
        TEST_ASSERT_TRUE(cJSON_IsFalse(member(params, "additionalProperties")));
        TEST_ASSERT_TRUE(cJSON_IsObject(member(params, "properties")));
        TEST_ASSERT_TRUE(cJSON_IsArray(member(params, "required")));
        const cJSON *result = member(entry, "result_schema");
        TEST_ASSERT_EQUAL_STRING("https://json-schema.org/draft/2020-12/schema", member(result, "$schema")->valuestring);
    }
    const cJSON *apply = member(cJSON_GetArrayItem(commands, AGENT_PROJECT_APPLY), "params_schema");
    TEST_ASSERT_TRUE(string_in_array(member(apply, "required"), "transaction"));
    const cJSON *envelope = member(member(apply, "properties"), "transaction");
    TEST_ASSERT_FALSE(cJSON_HasObjectItem(envelope, "$ref"));
    TEST_ASSERT_TRUE(cJSON_IsArray(member(envelope, "allOf")));
    const cJSON *close = member(cJSON_GetArrayItem(commands, AGENT_SESSION_CLOSE), "params_schema");
    TEST_ASSERT_TRUE(cJSON_IsArray(member(close, "allOf")));
    cJSON_Delete(help);
}

static void single_command_help_keeps_status_projection_complete(void) {
    char *text = agent_help_encode("session.status");
    TEST_ASSERT_NOT_NULL(text);
    cJSON *help = cJSON_Parse(text);
    free(text);
    TEST_ASSERT_NOT_NULL(help);
    const cJSON *commands = member(help, "commands");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(commands));
    const cJSON *result = member(cJSON_GetArrayItem(commands, 0), "result_schema");
    const cJSON *status = member(member(result, "$defs"), "status");
    const cJSON *props = member(status, "properties");
    static const char *const keys[] = {
        "state", "observed", "session_id", "host_generation", "host_kind", "canonical_path",
        "revision", "dirty", "event_sequence", "model_generation", "source_generation",
        "authorization", "controller_id", "recovery", "job"
    };
    TEST_ASSERT_EQUAL_INT(15, cJSON_GetArraySize(props));
    TEST_ASSERT_EQUAL_INT(15, cJSON_GetArraySize(member(status, "required")));
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        TEST_ASSERT_NOT_NULL(member(props, keys[i]));
        TEST_ASSERT_TRUE(string_in_array(member(status, "required"), keys[i]));
    }
    TEST_ASSERT_EQUAL_STRING("headless", member(member(props, "host_kind"), "const")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(member(member(props, "job"), "const")));
    cJSON_Delete(help);
    TEST_ASSERT_NULL(agent_help_encode("pack.start"));
}

static void transaction_help_inlines_core_schema_and_forbids_author(void) {
    char *core_text = tp_txn_request_schema_encode();
    TEST_ASSERT_NOT_NULL(core_text);
    cJSON *core = cJSON_Parse(core_text);
    free(core_text);
    TEST_ASSERT_NOT_NULL(core);
    TEST_ASSERT_EQUAL_STRING("https://json-schema.org/draft/2020-12/schema", member(core, "$schema")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(member(core, "additionalProperties")));
    const cJSON *root_props = member(core, "properties");
    TEST_ASSERT_EQUAL_INT(TP_TXN_SCHEMA, member(member(root_props, "schema"), "const")->valueint);
    TEST_ASSERT_TRUE(string_in_array(member(core, "required"), "schema"));
    TEST_ASSERT_TRUE(string_in_array(member(core, "required"), "transaction"));
    const cJSON *body = member(root_props, "transaction");
    TEST_ASSERT_TRUE(cJSON_IsFalse(member(body, "additionalProperties")));
    const cJSON *props = member(body, "properties");
    TEST_ASSERT_EQUAL_STRING("^[0-9a-f]{32}$", member(member(props, "id"), "pattern")->valuestring);
    TEST_ASSERT_EQUAL_STRING("integer", member(member(props, "expected_revision"), "type")->valuestring);
    TEST_ASSERT_EQUAL_INT(TP_TXN_MAX_OPS, member(member(props, "operations"), "maxItems")->valueint);
    TEST_ASSERT_TRUE(string_in_array(member(body, "required"), "id"));
    TEST_ASSERT_TRUE(string_in_array(member(body, "required"), "expected_revision"));
    TEST_ASSERT_TRUE(string_in_array(member(body, "required"), "operations"));
    TEST_ASSERT_FALSE(string_in_array(member(body, "required"), "label"));
    TEST_ASSERT_FALSE(string_in_array(member(body, "required"), "author"));
    TEST_ASSERT_EQUAL_STRING("string", member(member(props, "author"), "type")->valuestring);
    const cJSON *items = member(member(props, "operations"), "items");
    TEST_ASSERT_TRUE(string_in_array(member(items, "required"), "op"));

    char *help_text = agent_help_encode("project.apply");
    TEST_ASSERT_NOT_NULL(help_text);
    cJSON *help = cJSON_Parse(help_text);
    free(help_text);
    TEST_ASSERT_NOT_NULL(help);
    const cJSON *params = member(cJSON_GetArrayItem(member(help, "commands"), 0), "params_schema");
    const cJSON *branches = member(member(member(params, "properties"), "transaction"), "allOf");
    TEST_ASSERT_TRUE(cJSON_Compare(core, cJSON_GetArrayItem(branches, 0), true));
    const cJSON *restriction = member(member(cJSON_GetArrayItem(branches, 1), "properties"), "transaction");
    TEST_ASSERT_TRUE(string_in_array(member(member(restriction, "not"), "required"), "author"));
    TEST_ASSERT_TRUE(member(member(member(restriction, "properties"), "expected_revision"), "maximum")->valuedouble == 9007199254740991.0);
    cJSON_Delete(help);
    cJSON_Delete(core);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help-json") == 0) {
        char *text = agent_help_encode(NULL);
        if (!text) return 1;
        (void)puts(text);
        free(text);
        return 0;
    }
    UNITY_BEGIN();
    RUN_TEST(current_command_catalog_is_closed);
    RUN_TEST(flat_params_admit_only_documented_values);
    RUN_TEST(discard_and_history_require_safe_revision);
    RUN_TEST(help_exposes_command_and_result_shapes);
    RUN_TEST(single_command_help_keeps_status_projection_complete);
    RUN_TEST(transaction_help_inlines_core_schema_and_forbids_author);
    return UNITY_END();
}
