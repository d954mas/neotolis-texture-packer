#include "app_automation_policy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_json.h"
#include "nt_utf8_fs.h"
#include "tp_core/tp_identity.h"

#define APP_AUTOMATION_POLICY_MAX_BYTES (1024U * 1024U)

static tp_status read_document(const char *path, cJSON **out, tp_error *err) {
    FILE *file = nt_utf8_fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            return TP_STATUS_NOT_FOUND;
        }
        return tp_error_set(err, TP_STATUS_FILE_IO_FAILED,
                            "cannot open automation policy: %s", strerror(errno));
    }
    char *bytes = malloc(APP_AUTOMATION_POLICY_MAX_BYTES + 1U);
    if (!bytes) {
        (void)fclose(file);
        return tp_error_set(err, TP_STATUS_OOM,
                            "cannot allocate automation policy input");
    }
    const size_t length = fread(bytes, 1U, APP_AUTOMATION_POLICY_MAX_BYTES + 1U, file);
    const bool read_failed = ferror(file) != 0;
    const int close_result = fclose(file);
    tp_status status = TP_STATUS_OK;
    if (read_failed || close_result != 0) {
        status = tp_error_set(err, TP_STATUS_FILE_IO_FAILED,
                              "cannot read automation policy");
    } else if (length > APP_AUTOMATION_POLICY_MAX_BYTES) {
        status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                              "automation policy exceeds 1048576 bytes");
    } else {
        *out = app_json_parse(bytes, length, err);
        if (cJSON_IsObject(*out) && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(*out, "schema"))) {
            size_t schema_length = 0U;
            const char *schema = app_json_member_span(bytes, length, *out, "schema", &schema_length);
            if (!app_json_safe_integer(schema, schema_length)) {
                cJSON_Delete(*out);
                *out = NULL;
                tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "automation policy schema must be an exact integer");
            }
        }
        if (!*out) {
            status = TP_STATUS_INVALID_ARGUMENT;
        }
    }
    free(bytes);
    return status;
}

static bool project_decisions_valid(const cJSON *projects) {
    static const char *const fields[] = {"path", "decision"};
    if (!cJSON_IsArray(projects)) {
        return false;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, projects) {
        if (!app_json_object_fields(entry, fields, 2U)) {
            return false;
        }
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(entry, "path");
        const cJSON *decision = cJSON_GetObjectItemCaseSensitive(entry, "decision");
        if (!cJSON_IsString(path) || !cJSON_IsString(decision) ||
            (strcmp(decision->valuestring, "allow") != 0 &&
             strcmp(decision->valuestring, "deny") != 0)) {
            return false;
        }
        char lexical[TP_IDENTITY_PATH_MAX];
        if (tp_identity_path_lexical(path->valuestring, lexical, sizeof lexical,
                                     NULL) != TP_STATUS_OK) {
            return false;
        }
    }
    return true;
}

tp_status app_automation_policy_read(const char *data_root,
                                      app_automation_mode *mode,
                                      tp_error *err) {
    if (!mode) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "automation policy requires mode output");
    }
    *mode = APP_AUTOMATION_DISABLED;
    char root[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_lexical(data_root, root, sizeof root, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char path[TP_IDENTITY_PATH_MAX];
    const int length = snprintf(path, sizeof path, "%s/automation/permissions.json", root);
    if (length < 0 || (size_t)length >= sizeof path) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "automation policy path is too long");
    }

    cJSON *document = NULL;
    status = read_document(path, &document, err);
    if (status == TP_STATUS_NOT_FOUND) {
        *mode = APP_AUTOMATION_ASK;
        return TP_STATUS_OK;
    }
    if (status != TP_STATUS_OK) {
        return status;
    }
    static const char *const fields[] = {"schema", "mode", "projects"};
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(document, "schema");
    const cJSON *mode_value = cJSON_GetObjectItemCaseSensitive(document, "mode");
    const cJSON *projects = cJSON_GetObjectItemCaseSensitive(document, "projects");
    if (!app_json_object_fields(document, fields, 3U) ||
        !cJSON_IsNumber(schema) || schema->valuedouble != 1.0 ||
        !cJSON_IsString(mode_value) || !project_decisions_valid(projects)) {
        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                              "automation policy does not match schema 1");
    } else if (strcmp(mode_value->valuestring, "disabled") == 0) {
        *mode = APP_AUTOMATION_DISABLED;
    } else if (strcmp(mode_value->valuestring, "ask") == 0) {
        *mode = APP_AUTOMATION_ASK;
    } else if (strcmp(mode_value->valuestring, "allow_all") == 0) {
        *mode = APP_AUTOMATION_ALLOW_ALL;
    } else {
        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                              "automation policy has an unknown mode");
    }
    cJSON_Delete(document);
    return status;
}
