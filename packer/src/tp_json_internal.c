#include "tp_json_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tp_utf8_internal.h"

#define TP_JSON_SMALL_OBJECT_KEYS 16U

tp_status tp_json_reject_c_string_ambiguity(
    const char *json, size_t json_len, tp_status rejection_status,
    const char *context, tp_error *error) {
    const char *label = context ? context : "JSON";
    if (!json) {
        return tp_error_set(error, rejection_status, "%s is NULL", label);
    }

    const tp_status utf8_status = tp_utf8_validate_bytes(
        json, json_len, rejection_status, label, error);
    if (utf8_status != TP_STATUS_OK) {
        return utf8_status;
    }

    bool in_string = false;
    for (size_t i = 0U; i < json_len; ++i) {
        const unsigned char byte = (unsigned char)json[i];
        if (byte == 0U) {
            return tp_error_set(
                error, rejection_status,
                "%s contains a raw NUL byte at offset %zu", label, i);
        }
        if (!in_string) {
            if (byte == (unsigned char)'\"') {
                in_string = true;
            }
            continue;
        }
        if (byte == (unsigned char)'\"') {
            in_string = false;
            continue;
        }
        if (byte != (unsigned char)'\\') {
            continue;
        }
        if (i + 1U >= json_len) {
            break; /* malformed escape remains the parser's diagnostic */
        }
        const unsigned char escaped = (unsigned char)json[++i];
        if (escaped == (unsigned char)'u' && i + 4U < json_len &&
            json[i + 1U] == '0' && json[i + 2U] == '0' &&
            json[i + 3U] == '0' && json[i + 4U] == '0') {
            return tp_error_set(
                error, rejection_status,
                "%s contains escaped NUL (\\u0000) at offset %zu", label,
                i - 1U);
        }
    }
    return TP_STATUS_OK;
}

static int json_key_name_compare(const void *left, const void *right) {
    const char *const left_name = *(const char *const *)left;
    const char *const right_name = *(const char *const *)right;
    return strcmp(left_name, right_name);
}

static tp_status reject_duplicate_keys_recursive(
    const cJSON *node, tp_status rejection_status, const char *context,
    tp_error *error) {
    if (cJSON_IsObject(node)) {
        size_t key_count = 0U;
        for (const cJSON *child = node->child; child; child = child->next) {
            key_count++;
        }
        if (key_count > 1U) {
            const char *small_names[TP_JSON_SMALL_OBJECT_KEYS];
            const char **names = small_names;
            if (key_count > TP_JSON_SMALL_OBJECT_KEYS) {
                if (key_count > (size_t)-1 / sizeof *names) {
                    return tp_error_set(
                        error, TP_STATUS_OUT_OF_BOUNDS,
                        "%s object has too many keys", context);
                }
                names = (const char **)malloc(key_count * sizeof *names);
                if (!names) {
                    return tp_error_set(
                        error, TP_STATUS_OOM,
                        "%s duplicate-key validation allocation failed",
                        context);
                }
            }

            size_t index = 0U;
            for (const cJSON *child = node->child; child;
                 child = child->next) {
                names[index++] = child->string ? child->string : "";
            }
            qsort(names, key_count, sizeof *names, json_key_name_compare);
            for (size_t i = 1U; i < key_count; ++i) {
                if (strcmp(names[i - 1U], names[i]) == 0) {
                    const tp_status status = tp_error_set(
                        error, rejection_status,
                        "%s contains duplicate JSON key \"%s\"", context,
                        names[i]);
                    if (names != small_names) {
                        free(names);
                    }
                    return status;
                }
            }
            if (names != small_names) {
                free(names);
            }
        }
    }

    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (const cJSON *child = node->child; child; child = child->next) {
            const tp_status status = reject_duplicate_keys_recursive(
                child, rejection_status, context, error);
            if (status != TP_STATUS_OK) {
                return status;
            }
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_json_reject_duplicate_keys(
    const cJSON *root, tp_status rejection_status, const char *context,
    tp_error *error) {
    if (!root) {
        return tp_error_set(error, rejection_status, "%s root is NULL",
                            context ? context : "JSON");
    }
    return reject_duplicate_keys_recursive(
        root, rejection_status, context ? context : "JSON", error);
}
