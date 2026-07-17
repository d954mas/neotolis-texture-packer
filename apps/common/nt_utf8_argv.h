#ifndef NTPACKER_UTF8_ARGV_H
#define NTPACKER_UTF8_ARGV_H

#if defined(_WIN32)

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct nt_utf8_argv {
    int argc;
    char **argv;
} nt_utf8_argv;

/* Windows exposes the process command line as UTF-16. Both clients convert it
 * once at ingress so every path below this boundary is strict UTF-8. */
bool nt_utf8_argv_convert(int argc, wchar_t *const *wide_argv,
                          nt_utf8_argv *out, char *error,
                          size_t error_capacity);
bool nt_utf8_argv_from_command_line(nt_utf8_argv *out, char *error,
                                    size_t error_capacity);
void nt_utf8_argv_dispose(nt_utf8_argv *args);

/* Strict UTF-16 -> UTF-8 conversion shared by Windows client ingress paths. */
bool nt_win_utf16_to_utf8(const wchar_t *wide, char *out,
                          size_t output_capacity, char *error,
                          size_t error_capacity);

/* Windows path sources converted once to strict UTF-8. All return false on OS,
 * encoding, or capacity failure and leave a diagnostic in `error`. */
bool nt_win_current_directory_utf8(char *out, size_t output_capacity,
                                   char *error, size_t error_capacity);
bool nt_win_temp_path_utf8(char *out, size_t output_capacity, char *error,
                           size_t error_capacity);
bool nt_win_module_path_utf8(char *out, size_t output_capacity, char *error,
                             size_t error_capacity);
bool nt_win_environment_utf8(const wchar_t *name, char *out,
                             size_t output_capacity, bool *found, char *error,
                             size_t error_capacity);

#endif

#endif
