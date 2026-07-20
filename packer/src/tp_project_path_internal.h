#ifndef TP_PROJECT_PATH_INTERNAL_H
#define TP_PROJECT_PATH_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_project.h"

#define TP_PATH_MAX 4096

void tp_normalize_slashes(char *text);
bool tp_path_is_absolute(const char *path);
tp_status tp_abs_dir_of(const char *path, char *out, size_t cap);
tp_status tp_relativize(const char *absolute, const char *base_dir,
                        char *out, size_t cap);

#endif /* TP_PROJECT_PATH_INTERNAL_H */
