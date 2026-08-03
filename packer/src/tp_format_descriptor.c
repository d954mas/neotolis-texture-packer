#include "tp_format_descriptor_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_core/tp_export.h"
#include "tp_json_internal.h"
#include "tp_utf8_internal.h"

typedef struct tp_format_json_reader {
    const char *cursor;
    const char *end;
    size_t nodes;
} tp_format_json_reader;

struct tp_format_owned_descriptor {
    tp_format_descriptor view;
    tp_format_artifact_decl artifacts[TP_FORMAT_OUTPUT_MAX];
    tp_format_host_fact_decl host_facts[TP_FORMAT_HOST_FACT_MAX];
};

static char *format_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1U);
    if (copy) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static void parse_result_reject(tp_format_descriptor_parse_result *out,
                                tp_format_diagnostic_code code,
                                const char *message) {
    out->outcome = TP_FORMAT_DESCRIPTOR_REJECTED;
    out->rejection_code = code;
    if (message) {
        (void)snprintf(out->message, sizeof out->message, "%s", message);
        tp_error_trim_partial_utf8(out->message);
    }
}

static tp_status reject_from_error(tp_format_descriptor_parse_result *out,
                                   tp_format_diagnostic_code code,
                                   const tp_error *error,
                                   const char *fallback) {
    parse_result_reject(out, code,
                        error && error->msg[0] ? error->msg : fallback);
    return TP_STATUS_OK;
}

static bool byte_length_between(const char *text, size_t minimum,
                                size_t maximum) {
    if (!text) {
        return false;
    }
    const size_t length = strlen(text);
    return length >= minimum && length <= maximum;
}

static bool is_c0_or_c1(const unsigned char *text, size_t available,
                        size_t *width) {
    *width = tp_utf8_codepoint_width((const char *)text, available);
    if (*width == 0U) {
        return true;
    }
    if (*width == 1U) {
        return text[0] <= 0x1fU || text[0] == 0x7fU;
    }
    return *width == 2U && text[0] == 0xc2U &&
           text[1] >= 0x80U && text[1] <= 0x9fU;
}

static bool text_has_no_controls(const char *text) {
    const size_t length = text ? strlen(text) : 0U;
    for (size_t offset = 0U; offset < length;) {
        size_t width = 0U;
        if (is_c0_or_c1((const unsigned char *)text + offset,
                        length - offset, &width)) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool tp_format_id_is_runtime_token(const char *text) {
    if (!byte_length_between(text, 1U, TP_FORMAT_ID_MAX_BYTES) ||
        text[0] < 'a' || text[0] > 'z') {
        return false;
    }
    bool previous_hyphen = false;
    for (size_t i = 1U; text[i]; ++i) {
        const char c = text[i];
        const bool alnum = (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9');
        if (alnum) {
            previous_hyphen = false;
        } else if (c == '-' && !previous_hyphen) {
            previous_hyphen = true;
        } else {
            return false;
        }
    }
    return !previous_hyphen;
}

bool tp_format_logical_id_is_token(const char *text) {
    if (!byte_length_between(text, 1U, TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
        text[0] < 'a' || text[0] > 'z') {
        return false;
    }
    bool previous_underscore = false;
    for (size_t i = 1U; text[i]; ++i) {
        const char c = text[i];
        const bool alnum = (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9');
        if (alnum) {
            previous_underscore = false;
        } else if (c == '_' && !previous_underscore) {
            previous_underscore = true;
        } else {
            return false;
        }
    }
    return !previous_underscore;
}

bool tp_format_package_name_is_portable(const char *text) {
    if (!byte_length_between(text, 1U, TP_FORMAT_PACKAGE_NAME_MAX_BYTES) ||
        strcmp(text, ".") == 0 || strcmp(text, "..") == 0 ||
        !tp_utf8_is_valid_c_string(text) || !text_has_no_controls(text)) {
        return false;
    }
    return !strchr(text, '/') && !strchr(text, '\\') &&
           !strchr(text, ':');
}

static bool suffix_is_valid(const char *text) {
    if (!byte_length_between(text, 2U, TP_FORMAT_SUFFIX_MAX_BYTES) ||
        text[0] != '.' || !((text[1] >= 'a' && text[1] <= 'z') ||
                            (text[1] >= '0' && text[1] <= '9'))) {
        return false;
    }
    for (size_t i = 2U; text[i]; ++i) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
        if (c == '.' && text[i - 1U] == '.') {
            return false;
        }
    }
    return text[strlen(text) - 1U] != '.';
}

static bool json_is_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static void json_skip_whitespace(tp_format_json_reader *reader) {
    while (reader->cursor < reader->end) {
        if (!json_is_whitespace(*reader->cursor)) {
            break;
        }
        ++reader->cursor;
    }
}

static tp_status json_syntax_error(tp_error *error, const char *message) {
    return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                        "format JSON %s", message);
}

static int json_hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool json_take_hex4(tp_format_json_reader *reader,
                           uint32_t *value) {
    if ((size_t)(reader->end - reader->cursor) < 4U) {
        return false;
    }
    uint32_t parsed = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        const int digit = json_hex_value(reader->cursor[i]);
        if (digit < 0) {
            return false;
        }
        parsed = (parsed << 4U) | (uint32_t)digit;
    }
    reader->cursor += 4;
    *value = parsed;
    return true;
}

static tp_status json_parse_string(tp_format_json_reader *reader,
                                   tp_error *error) {
    if (reader->cursor >= reader->end || *reader->cursor != '"') {
        return json_syntax_error(error, "requires a string");
    }
    ++reader->cursor;
    while (reader->cursor < reader->end) {
        const unsigned char c = (unsigned char)*reader->cursor++;
        if (c == '"') {
            return TP_STATUS_OK;
        }
        if (c < 0x20U) {
            return json_syntax_error(error,
                                     "contains a raw control in a string");
        }
        if (c != '\\') {
            continue;
        }
        if (reader->cursor >= reader->end) {
            return json_syntax_error(error, "has a truncated escape");
        }
        const char escaped = *reader->cursor++;
        if (strchr("\"\\/bfnrt", escaped)) {
            continue;
        }
        if (escaped != 'u') {
            return json_syntax_error(error, "has an invalid escape");
        }
        uint32_t codepoint = 0U;
        if (!json_take_hex4(reader, &codepoint)) {
            return json_syntax_error(error,
                                     "has an invalid Unicode escape");
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
            if ((size_t)(reader->end - reader->cursor) < 6U ||
                reader->cursor[0] != '\\' || reader->cursor[1] != 'u') {
                return json_syntax_error(
                    error, "has an unpaired high surrogate");
            }
            reader->cursor += 2;
            uint32_t low = 0U;
            if (!json_take_hex4(reader, &low) || low < 0xdc00U ||
                low > 0xdfffU) {
                return json_syntax_error(
                    error, "has an invalid surrogate pair");
            }
        } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
            return json_syntax_error(error,
                                     "has an unpaired low surrogate");
        }
    }
    return json_syntax_error(error, "has an unterminated string");
}

static tp_status json_parse_value(tp_format_json_reader *reader,
                                  size_t depth, tp_error *error);

static tp_status json_count_container_entry(size_t *entries,
                                            tp_error *error) {
    if (*entries >= TP_FORMAT_JSON_CONTAINER_ENTRY_MAX) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format JSON container exceeds the entry limit");
    }
    ++*entries;
    return TP_STATUS_OK;
}

static tp_status json_parse_object(tp_format_json_reader *reader,
                                   size_t depth, tp_error *error) {
    ++reader->cursor; /* '{' */
    json_skip_whitespace(reader);
    if (reader->cursor < reader->end && *reader->cursor == '}') {
        ++reader->cursor;
        return TP_STATUS_OK;
    }
    size_t entries = 0U;
    for (;;) {
        tp_status status = json_count_container_entry(&entries, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        status = json_parse_string(reader, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        json_skip_whitespace(reader);
        if (reader->cursor >= reader->end || *reader->cursor++ != ':') {
            return json_syntax_error(error,
                                     "object member is missing ':'");
        }
        status = json_parse_value(reader, depth, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        json_skip_whitespace(reader);
        if (reader->cursor >= reader->end) {
            return json_syntax_error(error, "has an unterminated object");
        }
        const char delimiter = *reader->cursor++;
        if (delimiter == '}') {
            return TP_STATUS_OK;
        }
        if (delimiter != ',') {
            return json_syntax_error(error,
                                     "object members need ',' separators");
        }
        json_skip_whitespace(reader);
    }
}

static tp_status json_parse_array(tp_format_json_reader *reader,
                                  size_t depth, tp_error *error) {
    ++reader->cursor; /* '[' */
    json_skip_whitespace(reader);
    if (reader->cursor < reader->end && *reader->cursor == ']') {
        ++reader->cursor;
        return TP_STATUS_OK;
    }
    size_t entries = 0U;
    for (;;) {
        tp_status status = json_count_container_entry(&entries, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        status = json_parse_value(reader, depth, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        json_skip_whitespace(reader);
        if (reader->cursor >= reader->end) {
            return json_syntax_error(error, "has an unterminated array");
        }
        const char delimiter = *reader->cursor++;
        if (delimiter == ']') {
            return TP_STATUS_OK;
        }
        if (delimiter != ',') {
            return json_syntax_error(error,
                                     "array values need ',' separators");
        }
        json_skip_whitespace(reader);
    }
}

static bool json_take_literal(tp_format_json_reader *reader,
                              const char *literal) {
    const size_t length = strlen(literal);
    if ((size_t)(reader->end - reader->cursor) < length ||
        memcmp(reader->cursor, literal, length) != 0) {
        return false;
    }
    reader->cursor += length;
    return true;
}

static tp_status json_parse_number(tp_format_json_reader *reader,
                                   tp_error *error) {
    if (*reader->cursor == '-') {
        ++reader->cursor;
        if (reader->cursor >= reader->end) {
            return json_syntax_error(error, "has a truncated number");
        }
    }
    if (*reader->cursor == '0') {
        ++reader->cursor;
        if (reader->cursor < reader->end &&
            *reader->cursor >= '0' && *reader->cursor <= '9') {
            return json_syntax_error(error,
                                     "number has a leading zero");
        }
    } else if (*reader->cursor >= '1' && *reader->cursor <= '9') {
        do {
            ++reader->cursor;
        } while (reader->cursor < reader->end &&
                 *reader->cursor >= '0' && *reader->cursor <= '9');
    } else {
        return json_syntax_error(error, "has an invalid number");
    }
    if (reader->cursor < reader->end && *reader->cursor == '.') {
        ++reader->cursor;
        if (reader->cursor >= reader->end || *reader->cursor < '0' ||
            *reader->cursor > '9') {
            return json_syntax_error(error,
                                     "fraction requires digits");
        }
        do {
            ++reader->cursor;
        } while (reader->cursor < reader->end &&
                 *reader->cursor >= '0' && *reader->cursor <= '9');
    }
    if (reader->cursor < reader->end &&
        (*reader->cursor == 'e' || *reader->cursor == 'E')) {
        ++reader->cursor;
        if (reader->cursor < reader->end &&
            (*reader->cursor == '+' || *reader->cursor == '-')) {
            ++reader->cursor;
        }
        if (reader->cursor >= reader->end || *reader->cursor < '0' ||
            *reader->cursor > '9') {
            return json_syntax_error(error,
                                     "exponent requires digits");
        }
        do {
            ++reader->cursor;
        } while (reader->cursor < reader->end &&
                 *reader->cursor >= '0' && *reader->cursor <= '9');
    }
    return TP_STATUS_OK;
}

static tp_status json_parse_value(tp_format_json_reader *reader,
                                  size_t depth, tp_error *error) {
    json_skip_whitespace(reader);
    if (reader->cursor >= reader->end) {
        return json_syntax_error(error, "is missing a value");
    }
    if (reader->nodes >= TP_FORMAT_JSON_NODE_MAX) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format JSON exceeds the node limit");
    }
    ++reader->nodes;
    const char c = *reader->cursor;
    if (c == '{' || c == '[') {
        if (depth >= TP_FORMAT_JSON_DEPTH_MAX) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "format JSON exceeds the depth limit");
        }
        return c == '{' ? json_parse_object(reader, depth + 1U, error)
                        : json_parse_array(reader, depth + 1U, error);
    }
    if (c == '"') {
        return json_parse_string(reader, error);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(reader, error);
    }
    if (json_take_literal(reader, "true") ||
        json_take_literal(reader, "false") ||
        json_take_literal(reader, "null")) {
        return TP_STATUS_OK;
    }
    return json_syntax_error(error, "has an invalid value token");
}

static tp_status json_preflight(const char *text, size_t length,
                                tp_error *error) {
    if (length > TP_FORMAT_DESCRIPTOR_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format descriptor exceeds 65536 bytes");
    }
    if (length >= 3U && (unsigned char)text[0] == 0xefU &&
        (unsigned char)text[1] == 0xbbU &&
        (unsigned char)text[2] == 0xbfU) {
        return tp_error_set(error, TP_STATUS_INVALID_UTF8,
                            "format descriptor must not contain a UTF-8 BOM");
    }
    tp_status status = tp_json_reject_c_string_ambiguity(
        text, length, TP_STATUS_INVALID_UTF8, "format descriptor", error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_format_json_reader reader = {
        .cursor = text,
        .end = text + length,
    };
    status = json_parse_value(&reader, 0U, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    json_skip_whitespace(&reader);
    return reader.cursor == reader.end
               ? TP_STATUS_OK
               : json_syntax_error(error, "contains trailing data");
}

static bool object_has_exact_keys(const cJSON *object,
                                  const char *const *required,
                                  size_t required_count,
                                  const char *const *optional,
                                  size_t optional_count) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (size_t i = 0U; i < required_count; ++i) {
        if (!cJSON_GetObjectItemCaseSensitive(object, required[i])) {
            return false;
        }
    }
    for (const cJSON *item = object->child; item; item = item->next) {
        bool known = false;
        for (size_t i = 0U; i < required_count; ++i) {
            known = known || strcmp(item->string, required[i]) == 0;
        }
        for (size_t i = 0U; i < optional_count; ++i) {
            known = known || strcmp(item->string, optional[i]) == 0;
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

static const char *skip_json_string(const char *cursor, const char *end) {
    if (cursor >= end || *cursor != '"') {
        return NULL;
    }
    ++cursor;
    while (cursor < end) {
        if (*cursor == '\\') {
            cursor += cursor + 1 < end ? 2 : 1;
        } else if (*cursor++ == '"') {
            return cursor;
        }
    }
    return NULL;
}

static const char *skip_json_value(const char *cursor, const char *end) {
    while (cursor < end && json_is_whitespace(*cursor)) {
        ++cursor;
    }
    if (cursor >= end) {
        return NULL;
    }
    if (*cursor == '"') {
        return skip_json_string(cursor, end);
    }
    if (*cursor == '{' || *cursor == '[') {
        const char open = *cursor;
        const char close = open == '{' ? '}' : ']';
        size_t depth = 0U;
        for (; cursor < end; ++cursor) {
            if (*cursor == '"') {
                cursor = skip_json_string(cursor, end);
                if (!cursor) {
                    return NULL;
                }
                --cursor;
            } else if (*cursor == open) {
                ++depth;
            } else if (*cursor == close && --depth == 0U) {
                return cursor + 1;
            }
        }
        return NULL;
    }
    while (cursor < end && *cursor != ',' && *cursor != '}' &&
           *cursor != ']') {
        ++cursor;
    }
    return cursor;
}

/* cJSON preserves object insertion order. Pair its decoded child index with the
 * raw value span so escaped key spellings still receive lexical integer rules. */
static bool root_value_span(const char *text, size_t length,
                            size_t wanted_index, const char **value_start,
                            const char **value_end) {
    const char *cursor = text;
    const char *end = text + length;
    while (cursor < end && json_is_whitespace(*cursor)) {
        ++cursor;
    }
    if (cursor >= end || *cursor++ != '{') {
        return false;
    }
    size_t index = 0U;
    for (;;) {
        while (cursor < end && json_is_whitespace(*cursor)) {
            ++cursor;
        }
        if (cursor >= end || *cursor == '}') {
            return false;
        }
        cursor = skip_json_string(cursor, end);
        if (!cursor) {
            return false;
        }
        while (cursor < end && json_is_whitespace(*cursor)) {
            ++cursor;
        }
        if (cursor >= end || *cursor++ != ':') {
            return false;
        }
        while (cursor < end && json_is_whitespace(*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        const char *after = skip_json_value(cursor, end);
        if (!after) {
            return false;
        }
        const char *trimmed = after;
        while (trimmed > start && json_is_whitespace(trimmed[-1])) {
            --trimmed;
        }
        if (index == wanted_index) {
            *value_start = start;
            *value_end = trimmed;
            return true;
        }
        ++index;
        cursor = after;
        while (cursor < end && json_is_whitespace(*cursor)) {
            ++cursor;
        }
        if (cursor >= end || *cursor != ',') {
            return false;
        }
        ++cursor;
    }
}

static bool parse_api_integer_token(const char *start, const char *end,
                                     uint32_t *value, bool *unsupported) {
    *unsupported = false;
    if (!start || start == end) {
        return false;
    }
    const bool negative = *start == '-';
    const char *cursor = negative ? start + 1 : start;
    if (cursor == end) {
        return false;
    }
    uint32_t parsed = 0U;
    for (; cursor < end; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (negative) {
            continue;
        }
        if (parsed > (UINT32_MAX - digit) / 10U) {
            *unsupported = true;
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (negative) {
        if (end - start == 2 && start[1] == '0') {
            return false;
        }
        *unsupported = true;
        return false;
    }
    *value = parsed;
    return true;
}

static int transform_token_value(const char *token) {
    static const char *const tokens[] = {
        "identity", "flip_h", "flip_v", "rotate_180", "transpose",
        "rotate_90_cw", "rotate_90_ccw", "anti_transpose",
    };
    for (int i = 0; i < (int)(sizeof tokens / sizeof tokens[0]); ++i) {
        if (strcmp(token, tokens[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parse_capabilities(const cJSON *object, tp_export_caps *out) {
    static const char *const keys[] = {
        "transforms", "polygons", "pivot", "slice9", "multipage",
        "aliases", "animations",
    };
    if (!object_has_exact_keys(object, keys,
                               sizeof keys / sizeof keys[0], NULL, 0U)) {
        return false;
    }
    const cJSON *transforms =
        cJSON_GetObjectItemCaseSensitive(object, "transforms");
    const int transform_count = cJSON_GetArraySize(transforms);
    if (!cJSON_IsArray(transforms) || transform_count < 1 ||
        transform_count > 8) {
        return false;
    }
    int previous = -1;
    uint8_t mask = 0U;
    for (int i = 0; i < transform_count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(transforms, i);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return false;
        }
        const int value = transform_token_value(item->valuestring);
        if (value < 0 || value <= previous || (i == 0 && value != 0)) {
            return false;
        }
        previous = value;
        mask = (uint8_t)(mask | TP_EXPORT_TRANSFORM_BIT(value));
    }
    const cJSON *polygons =
        cJSON_GetObjectItemCaseSensitive(object, "polygons");
    const cJSON *pivot = cJSON_GetObjectItemCaseSensitive(object, "pivot");
    const cJSON *slice9 = cJSON_GetObjectItemCaseSensitive(object, "slice9");
    const cJSON *multipage =
        cJSON_GetObjectItemCaseSensitive(object, "multipage");
    const cJSON *aliases = cJSON_GetObjectItemCaseSensitive(object, "aliases");
    const cJSON *animations =
        cJSON_GetObjectItemCaseSensitive(object, "animations");
    if (!cJSON_IsBool(polygons) || !cJSON_IsBool(pivot) ||
        !cJSON_IsBool(slice9) || !cJSON_IsBool(multipage) ||
        !cJSON_IsBool(aliases) || !cJSON_IsBool(animations)) {
        return false;
    }
    *out = (tp_export_caps){
        .transform_mask = mask,
        .polygons = cJSON_IsTrue(polygons),
        .pivot = cJSON_IsTrue(pivot),
        .slice9 = cJSON_IsTrue(slice9),
        .multipage = cJSON_IsTrue(multipage),
        .aliases = cJSON_IsTrue(aliases),
        .animations = cJSON_IsTrue(animations),
    };
    return true;
}

static bool parse_outputs(const cJSON *array,
                          tp_format_owned_descriptor *owned,
                          tp_format_diagnostic_code *code, bool *oom) {
    *oom = false;
    const int count = cJSON_GetArraySize(array);
    if (!cJSON_IsArray(array) || count < 1 ||
        count > (int)TP_FORMAT_OUTPUT_MAX) {
        *code = TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID;
        return false;
    }
    owned->view.artifacts = owned->artifacts;
    owned->view.artifact_count = count;
    static const char *const keys[] = {"id", "suffix"};
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (!object_has_exact_keys(item, keys, 2U, NULL, 0U)) {
            *code = TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID;
            return false;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *suffix =
            cJSON_GetObjectItemCaseSensitive(item, "suffix");
        if (!cJSON_IsString(id) || !id->valuestring ||
            !tp_format_logical_id_is_token(id->valuestring) ||
            !cJSON_IsString(suffix) || !suffix->valuestring ||
            !suffix_is_valid(suffix->valuestring)) {
            *code = TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID;
            return false;
        }
        for (int j = 0; j < i; ++j) {
            if (strcmp(owned->artifacts[j].id, id->valuestring) == 0 ||
                strcmp(owned->artifacts[j].suffix, suffix->valuestring) == 0) {
                *code = TP_FORMAT_DIAGNOSTIC_OUTPUT_CONFLICT;
                return false;
            }
        }
        owned->artifacts[i].id = format_strdup(id->valuestring);
        owned->artifacts[i].suffix = format_strdup(suffix->valuestring);
        if (!owned->artifacts[i].id || !owned->artifacts[i].suffix) {
            *oom = true;
            return false;
        }
    }
    return true;
}

static bool output_exists(const tp_format_owned_descriptor *owned,
                          const char *id) {
    for (int i = 0; i < owned->view.artifact_count; ++i) {
        if (strcmp(owned->artifacts[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_host_facts(const cJSON *array,
                             tp_format_owned_descriptor *owned, bool *oom) {
    *oom = false;
    if (!array) {
        return true;
    }
    const int count = cJSON_GetArraySize(array);
    if (!cJSON_IsArray(array) || count < 0 ||
        count > (int)TP_FORMAT_HOST_FACT_MAX) {
        return false;
    }
    owned->view.host_facts = count > 0 ? owned->host_facts : NULL;
    owned->view.host_fact_count = count;
    static const char *const keys[] = {
        "id", "kind", "output", "root_marker", "missing",
    };
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (!object_has_exact_keys(item, keys, 5U, NULL, 0U)) {
            return false;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        const cJSON *output =
            cJSON_GetObjectItemCaseSensitive(item, "output");
        const cJSON *marker =
            cJSON_GetObjectItemCaseSensitive(item, "root_marker");
        const cJSON *missing =
            cJSON_GetObjectItemCaseSensitive(item, "missing");
        if (!cJSON_IsString(id) || !id->valuestring ||
            !tp_format_logical_id_is_token(id->valuestring) ||
            !cJSON_IsString(kind) ||
            strcmp(kind->valuestring, "project_resource") != 0 ||
            !cJSON_IsString(output) || !output_exists(owned, output->valuestring) ||
            !cJSON_IsString(marker) ||
            strcmp(marker->valuestring, "game.project") != 0 ||
            !cJSON_IsString(missing) ||
            strcmp(missing->valuestring, "basename_notice") != 0) {
            return false;
        }
        for (int j = 0; j < i; ++j) {
            if (strcmp(owned->host_facts[j].id, id->valuestring) == 0) {
                return false;
            }
        }
        tp_format_host_fact_decl *fact = &owned->host_facts[i];
        fact->id = format_strdup(id->valuestring);
        fact->kind = TP_FORMAT_HOST_FACT_PROJECT_RESOURCE;
        fact->output_id = format_strdup(output->valuestring);
        fact->root_marker = format_strdup(marker->valuestring);
        fact->missing = TP_FORMAT_HOST_FACT_MISSING_BASENAME_NOTICE;
        if (!fact->id || !fact->output_id || !fact->root_marker) {
            *oom = true;
            return false;
        }
    }
    return true;
}

static bool native_id_reserved(const char *id) {
    return tp_format_catalog_find_available(tp_format_catalog_native(), id) !=
           NULL;
}

const tp_format_descriptor *tp_format_owned_descriptor_view(
    const tp_format_owned_descriptor *descriptor) {
    return descriptor ? &descriptor->view : NULL;
}

void tp_format_owned_descriptor_destroy(
    tp_format_owned_descriptor *descriptor) {
    if (!descriptor) {
        return;
    }
    free((char *)descriptor->view.id);
    free((char *)descriptor->view.display_name);
    for (int i = 0; i < descriptor->view.artifact_count; ++i) {
        free((char *)descriptor->artifacts[i].id);
        free((char *)descriptor->artifacts[i].suffix);
    }
    for (int i = 0; i < descriptor->view.host_fact_count; ++i) {
        free((char *)descriptor->host_facts[i].id);
        free((char *)descriptor->host_facts[i].output_id);
        free((char *)descriptor->host_facts[i].root_marker);
    }
    free(descriptor);
}

tp_status tp_format_descriptor_v1_parse(
    const unsigned char *bytes, size_t byte_count,
    tp_format_descriptor_parse_result *out, tp_error *error) {
    if (!bytes || !out) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format descriptor parser requires bytes and output");
    }
    memset(out, 0, sizeof *out);
    if (byte_count > TP_FORMAT_DESCRIPTOR_MAX_BYTES) {
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON,
                            "format descriptor exceeds 65536 bytes");
        return TP_STATUS_OK;
    }
    tp_error local = {{0}};
    tp_status status = json_preflight((const char *)bytes, byte_count, &local);
    if (status != TP_STATUS_OK) {
        const tp_format_diagnostic_code code =
            status == TP_STATUS_INVALID_UTF8
                ? TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_UTF8
                : TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON;
        return reject_from_error(out, code, &local,
                                 "format descriptor text is invalid");
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)bytes, byte_count,
                                           &parse_end, 0);
    if (!root) {
        return tp_error_set(
            error, TP_STATUS_OOM,
            "format descriptor JSON materialization failed after syntax validation");
    }
    const char *text_end = (const char *)bytes + byte_count;
    while (parse_end && parse_end < text_end &&
           json_is_whitespace(*parse_end)) {
        ++parse_end;
    }
    if (!parse_end || parse_end != text_end) {
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON,
                            "format descriptor contains trailing data");
        return TP_STATUS_OK;
    }
    status = tp_json_reject_duplicate_keys(
        root, TP_STATUS_INVALID_ARGUMENT, "format descriptor", &local);
    if (status == TP_STATUS_OOM) {
        cJSON_Delete(root);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format descriptor duplicate-key validation OOM");
    }
    if (status != TP_STATUS_OK) {
        cJSON_Delete(root);
        return reject_from_error(out,
                                 TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON,
                                 &local, "format descriptor has duplicate keys");
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
                            "format descriptor root must be an object");
        return TP_STATUS_OK;
    }

    const cJSON *api = cJSON_GetObjectItemCaseSensitive(root, "api_version");
    size_t api_index = 0U;
    bool api_found = false;
    for (const cJSON *item = root->child; item; item = item->next, ++api_index) {
        if (strcmp(item->string, "api_version") == 0) {
            api_found = true;
            break;
        }
    }
    const char *api_start = NULL;
    const char *api_end = NULL;
    uint32_t api_version = 0U;
    bool api_unsupported = false;
    if (!api_found || !api ||
        !root_value_span((const char *)bytes, byte_count, api_index,
                         &api_start, &api_end) ||
        !parse_api_integer_token(api_start, api_end, &api_version,
                                 &api_unsupported)) {
        cJSON_Delete(root);
        parse_result_reject(
            out,
            api_unsupported ? TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED
                            : TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
            api_unsupported ? "api_version integer is unsupported"
                            : "api_version must be one integer token");
        return TP_STATUS_OK;
    }
    if (api_version != TP_FORMAT_API_VERSION) {
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED,
                            "format descriptor api_version is unsupported");
        return TP_STATUS_OK;
    }

    static const char *const required[] = {
        "api_version", "id", "display_name", "capabilities", "outputs",
    };
    static const char *const optional[] = {"host_facts"};
    if (!object_has_exact_keys(root, required, 5U, optional, 1U)) {
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
                            "format descriptor has missing or unknown members");
        return TP_STATUS_OK;
    }

    tp_format_owned_descriptor *owned =
        (tp_format_owned_descriptor *)calloc(1, sizeof *owned);
    if (!owned) {
        cJSON_Delete(root);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format descriptor allocation failed");
    }
    owned->view.api_version = api_version;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *display =
        cJSON_GetObjectItemCaseSensitive(root, "display_name");
    if (!cJSON_IsString(id) || !id->valuestring ||
        !tp_format_id_is_runtime_token(id->valuestring)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_FORMAT_ID_INVALID,
                            "format id does not match the API-v1 grammar");
        return TP_STATUS_OK;
    }
    if (native_id_reserved(id->valuestring)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_FORMAT_ID_RESERVED,
                            "format id is reserved by a native format");
        return TP_STATUS_OK;
    }
    if (!cJSON_IsString(display) || !display->valuestring ||
        !byte_length_between(display->valuestring, 1U,
                             TP_FORMAT_DISPLAY_NAME_MAX_BYTES) ||
        !text_has_no_controls(display->valuestring)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
                            "display_name is not admitted API-v1 text");
        return TP_STATUS_OK;
    }
    owned->view.id = format_strdup(id->valuestring);
    owned->view.display_name = format_strdup(display->valuestring);
    if (!owned->view.id || !owned->view.display_name) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format descriptor string allocation failed");
    }
    if (!parse_capabilities(
            cJSON_GetObjectItemCaseSensitive(root, "capabilities"),
            &owned->view.caps)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
                            "capabilities has an invalid API-v1 shape");
        return TP_STATUS_OK;
    }
    tp_format_diagnostic_code output_code =
        TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID;
    bool output_oom = false;
    if (!parse_outputs(cJSON_GetObjectItemCaseSensitive(root, "outputs"),
                       owned, &output_code, &output_oom)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        if (output_oom) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format output allocation failed");
        }
        parse_result_reject(out, output_code,
                            output_code == TP_FORMAT_DIAGNOSTIC_OUTPUT_CONFLICT
                                ? "format outputs conflict"
                                : "format outputs are invalid");
        return TP_STATUS_OK;
    }
    bool fact_oom = false;
    if (!parse_host_facts(
            cJSON_GetObjectItemCaseSensitive(root, "host_facts"), owned,
            &fact_oom)) {
        tp_format_owned_descriptor_destroy(owned);
        cJSON_Delete(root);
        if (fact_oom) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format host-fact allocation failed");
        }
        parse_result_reject(out, TP_FORMAT_DIAGNOSTIC_HOST_FACT_INVALID,
                            "format host_facts are invalid");
        return TP_STATUS_OK;
    }
    cJSON_Delete(root);
    out->outcome = TP_FORMAT_DESCRIPTOR_ADMITTED;
    out->owned_descriptor = owned;
    return TP_STATUS_OK;
}
