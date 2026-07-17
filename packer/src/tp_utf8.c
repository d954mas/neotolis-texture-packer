#include "tp_utf8_internal.h"

#include <stdbool.h>
#include <string.h>

static bool utf8_continuation(unsigned char byte) {
    return byte >= 0x80U && byte <= 0xbfU;
}

size_t tp_utf8_codepoint_width(const char *text, size_t available) {
    if (!text || available == 0U) {
        return 0U;
    }

    const unsigned char lead = (unsigned char)text[0];
    if (lead <= 0x7fU) {
        return 1U;
    }
    if (lead >= 0xc2U && lead <= 0xdfU) {
        return available >= 2U &&
                       utf8_continuation((unsigned char)text[1])
                   ? 2U
                   : 0U;
    }
    if (lead >= 0xe0U && lead <= 0xefU) {
        if (available < 3U) {
            return 0U;
        }
        const unsigned char second = (unsigned char)text[1];
        const unsigned char third = (unsigned char)text[2];
        const bool second_valid =
            (lead == 0xe0U) ? (second >= 0xa0U && second <= 0xbfU)
            : (lead == 0xedU) ? (second >= 0x80U && second <= 0x9fU)
                             : utf8_continuation(second);
        return second_valid && utf8_continuation(third) ? 3U : 0U;
    }
    if (lead >= 0xf0U && lead <= 0xf4U) {
        if (available < 4U) {
            return 0U;
        }
        const unsigned char second = (unsigned char)text[1];
        const unsigned char third = (unsigned char)text[2];
        const unsigned char fourth = (unsigned char)text[3];
        const bool second_valid =
            (lead == 0xf0U) ? (second >= 0x90U && second <= 0xbfU)
            : (lead == 0xf4U) ? (second >= 0x80U && second <= 0x8fU)
                             : utf8_continuation(second);
        return second_valid && utf8_continuation(third) &&
                       utf8_continuation(fourth)
                   ? 4U
                   : 0U;
    }
    return 0U;
}

bool tp_utf8_is_valid_bytes(const char *text, size_t length) {
    if (!text) {
        return false;
    }
    for (size_t offset = 0U; offset < length;) {
        const size_t width =
            tp_utf8_codepoint_width(text + offset, length - offset);
        if (width == 0U) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool tp_utf8_is_valid_c_string(const char *text) {
    return text && tp_utf8_is_valid_bytes(text, strlen(text));
}

tp_status tp_utf8_validate_bytes(const char *text, size_t length,
                                 tp_status rejection_status,
                                 const char *context, tp_error *error) {
    const char *label = context ? context : "text";
    if (!text) {
        return tp_error_set(error, rejection_status, "%s is NULL", label);
    }
    for (size_t i = 0U; i < length;) {
        const size_t width = tp_utf8_codepoint_width(text + i, length - i);
        if (width == 0U) {
            return tp_error_set(error, rejection_status,
                                "%s contains invalid UTF-8 at offset %zu",
                                label, i);
        }
        i += width;
    }
    return TP_STATUS_OK;
}

tp_status tp_utf8_validate_c_string(const char *text,
                                    tp_status rejection_status,
                                    const char *context, tp_error *error) {
    if (!text) {
        return tp_error_set(error, rejection_status, "%s is NULL",
                            context ? context : "text");
    }
    return tp_utf8_validate_bytes(text, strlen(text), rejection_status,
                                  context, error);
}
