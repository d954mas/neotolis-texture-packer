#include "app_json.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "core/nt_assert.h"
#include "tp_core/tp_json_text.h"

static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
static bool space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool app_json_safe_integer(const char *number, size_t length) {
    if (!number || length == 0U) return false;
    size_t p = number[0] == '-' ? 1U : 0U;
    bool fraction = false;
    int64_t fractional_digits = 0;
    size_t first = length, last = length, significant = 0U;
    for (; p < length && number[p] != 'e' && number[p] != 'E'; ++p) {
        if (number[p] == '.') { fraction = true; continue; }
        if (!digit(number[p])) return false;
        if (fraction) ++fractional_digits;
        if (number[p] != '0') {
            if (first == length) first = p;
            last = p;
        }
    }
    const size_t mantissa_end = p;
    int64_t exponent = 0;
    if (p < length) {
        ++p;
        const bool negative = p < length && number[p] == '-';
        if (p < length && (number[p] == '-' || number[p] == '+')) ++p;
        /* Saturation is enough to decide integrality/range, even for an
         * adversarial exponent occupying the rest of the bounded request. */
        const int64_t bound = (int64_t)length + 20;
        for (; p < length; ++p) {
            if (!digit(number[p])) return false;
            if (exponent < bound) exponent = exponent * 10 + number[p] - '0';
        }
        if (negative) exponent = -exponent;
    }
    if (first == length) return true; /* zero, including -0 and 0e-anything */
    if (number[0] == '-') return false;
    int64_t scale = exponent - fractional_digits;
    for (size_t i = last + 1U; i < mantissa_end; ++i) if (digit(number[i])) ++scale;
    for (size_t i = first; i <= last; ++i) if (digit(number[i])) ++significant;
    if (scale < 0 || scale > 16 || significant + (size_t)scale > 16U) return false;
    uint64_t integer = 0U;
    for (size_t i = first; i <= last; ++i) {
        if (digit(number[i])) integer = integer * 10U + (unsigned)(number[i] - '0');
    }
    while (scale-- > 0) integer *= 10U;
    return integer <= UINT64_C(9007199254740991);
}

/* cJSON accepts spellings such as 01 and 1. as numbers. Reject them before
 * parsing, while leaving structural syntax and escaped Unicode to cJSON. */
static bool strict_tokens(const char *bytes, size_t length) {
    bool quoted = false;
    for (size_t i = 0U; i < length; ++i) {
        const char ch = bytes[i];
        if (quoted) {
            if ((unsigned char)ch < 0x20U) return false;
            if (ch == '\\') ++i;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') { quoted = true; continue; }
        if ((unsigned char)ch < 0x20U && !space(ch)) return false;
        if (ch != '-' && !digit(ch)) continue;
        size_t p = i;
        if (bytes[p] == '-') ++p;
        if (p == length || !digit(bytes[p])) return false;
        if (bytes[p] == '0') ++p;
        else while (p < length && digit(bytes[p])) ++p;
        if (p < length && bytes[p] == '.') {
            ++p;
            if (p == length || !digit(bytes[p])) return false;
            while (p < length && digit(bytes[p])) ++p;
        }
        if (p < length && (bytes[p] == 'e' || bytes[p] == 'E')) {
            ++p;
            if (p < length && (bytes[p] == '+' || bytes[p] == '-')) ++p;
            if (p == length || !digit(bytes[p])) return false;
            while (p < length && digit(bytes[p])) ++p;
        }
        if (p < length && !space(bytes[p]) && bytes[p] != ',' &&
            bytes[p] != '}' && bytes[p] != ']') return false;
        i = p - 1U;
    }
    return !quoted;
}

static bool finite_numbers(const cJSON *node) {
    if (cJSON_IsNumber(node) && !isfinite(node->valuedouble)) return false;
    for (const cJSON *child = node->child; child; child = child->next) {
        if (!finite_numbers(child)) return false;
    }
    return true;
}

cJSON *app_json_parse(const char *bytes, size_t length, tp_error *err) {
    if (tp_json_reject_c_string_ambiguity(bytes, length,
            TP_STATUS_INVALID_ARGUMENT, "JSON", err) != TP_STATUS_OK) return NULL;
    if (!strict_tokens(bytes, length) ||
        (length >= 3U && memcmp(bytes, "\xef\xbb\xbf", 3U) == 0)) {
        tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid JSON token");
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(bytes, length, &end, 0);
    size_t consumed = root ? (size_t)(end - bytes) : 0U;
    while (consumed < length && space(bytes[consumed])) ++consumed;
    if (!root || consumed != length || !finite_numbers(root)) {
        cJSON_Delete(root);
        tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid JSON document");
        return NULL;
    }
    return root;
}

bool app_json_object_fields(const cJSON *object,
                            const char *const *keys, size_t count) {
    NT_ASSERT(count <= 64U);
    if (!cJSON_IsObject(object)) return false;
    uint64_t seen = 0U;
    for (const cJSON *item = object->child; item; item = item->next) {
        size_t index = 0U;
        while (index < count && strcmp(item->string, keys[index]) != 0) ++index;
        if (index == count || (seen & (UINT64_C(1) << index)) != 0U) return false;
        seen |= UINT64_C(1) << index;
    }
    return true;
}

static const char *value_end(const char *at, const char *end) {
    bool quoted = false;
    unsigned depth = 0U;
    const bool string_value = at < end && *at == '"';
    for (const char *p = at; p < end; ++p) {
        if (quoted) {
            if (*p == '\\') ++p;
            else if (*p == '"') {
                quoted = false;
                if (string_value && depth == 0U) return p + 1;
            }
        } else if (*p == '"') quoted = true;
        else if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') {
            if (depth == 0U) return p;
            if (--depth == 0U) return p + 1;
        } else if (depth == 0U && (*p == ',' || space(*p))) return p;
    }
    return end;
}

const char *app_json_member_span(const char *bytes, size_t length,
    const cJSON *object, const char *key, size_t *value_length) {
    *value_length = 0U;
    NT_ASSERT(cJSON_IsObject(object));
    const char *end = bytes + length;
    const char *cursor = bytes;
    while (cursor < end && space(*cursor)) ++cursor;
    NT_ASSERT(cursor < end && *cursor == '{');
    ++cursor;
    for (const cJSON *child = object->child; child; child = child->next) {
        while (cursor < end && (space(*cursor) || *cursor == ',')) ++cursor;
        cursor = value_end(cursor, end); /* key, including escaped spelling */
        while (cursor < end && space(*cursor)) ++cursor;
        NT_ASSERT(cursor < end && *cursor == ':');
        ++cursor;
        while (cursor < end && space(*cursor)) ++cursor;
        const char *after = value_end(cursor, end);
        if (strcmp(child->string, key) == 0) {
            *value_length = (size_t)(after - cursor);
            return cursor;
        }
        cursor = after;
    }
    return NULL;
}
