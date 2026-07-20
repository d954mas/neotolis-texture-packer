#ifndef TP_SOURCE_PATH_TEXT_INTERNAL_H
#define TP_SOURCE_PATH_TEXT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"

/* Stored and incoming source-path text shares one byte-level contract. This
 * module deliberately does not resolve roots, fold dot components, touch the
 * filesystem, normalize Unicode, or apply host case rules. */
#define TP_SOURCE_PATH_TEXT_CAP 4096U

tp_status tp_source_path_text_admit(const char *path);
tp_status tp_source_path_text_normalize(const char *path, char *out,
                                        size_t capacity);
uint64_t tp_source_path_text_hash(const char *admitted_path);
bool tp_source_path_text_equal(const char *left, const char *right);

#endif /* TP_SOURCE_PATH_TEXT_INTERNAL_H */
