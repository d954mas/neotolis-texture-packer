#ifndef TP_CORE_TP_UTF8_H
#define TP_CORE_TP_UTF8_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the byte width of the first strict Unicode-scalar UTF-8 code point,
 * or zero when the prefix is malformed or truncated. */
size_t tp_utf8_codepoint_width(const char *text, size_t available);

bool tp_utf8_is_valid_bytes(const char *text, size_t length);
bool tp_utf8_is_valid_c_string(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_UTF8_H */
